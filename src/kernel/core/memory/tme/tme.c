/*
 * TME / TME-MK runtime — KeyID pool, PCONFIG wrapper, init.
 *
 * Intel SDM Vol 3D Chap 16; Vol 2D PCONFIG; Intel TME-MK Spec 1.4.
 *
 * See tme.h for the architectural rationale (hybrid pre-program +
 * re-key-on-free pool). This file implements the runtime: state
 * capture, KeyID pool bitmap, PCONFIG operand construction, and Touch
 * publish on lifecycle events.
 */

#include "tme.h"
#include "vmm.h"
#include "memtag.h"
#include "klib.h"
#include "touch.h"
#include "debug.h"
#include "cpuid.h"

/* ─── Public state ─────────────────────────────────────────────────── */

TmeState g_tme = { 0 };

/* ─── Pool ─────────────────────────────────────────────────────────── */

/* Bitmap: 1 = slot in use OR unavailable (programming failed); 0 = free
 * and usable. KeyID 0 is permanently marked in-use (reserved platform
 * default). Slots above max_keyid stay marked in-use forever. */
#define TME_BITMAP_WORDS  (TME_MAX_KEYIDS / 64u)
static uint64_t   g_tme_bitmap[TME_BITMAP_WORDS];
static spinlock_t g_tme_lock;
static spinlock_t g_tme_zero_lock;
static bool       g_tme_initialized = false;

/* Pre-resolved Touch tag IDs — intern once on init to avoid hitting
 * the tag registry from off-hot paths. TouchTag = uint16_t with
 * TOUCH_TAG_INVALID = 0xFFFF. */
static TouchTag   g_tme_tag_pool_ready    = TOUCH_TAG_INVALID;
static TouchTag   g_tme_tag_rekey_failed  = TOUCH_TAG_INVALID;

/* ─── PCONFIG operand ──────────────────────────────────────────────── */

/* Intel TME-MK Spec 1.4 §4.2 "MKTME_KEY_PROGRAM_STRUCT":
 *   off    size  field
 *     0    2     KEYID
 *     2    4     KEYID_CTRL  (bits[3:0]=command, bits[31:24]=enc_alg)
 *     6   58     Reserved
 *    64   64     KEY_FIELD_1 (data key)
 *   128   64     KEY_FIELD_2 (tweak key)
 *   192   64     Reserved
 *   --- 256 total, 256-byte aligned --------
 *
 * SET_KEY_RANDOM ignores KEY_FIELD_1/2 (CPU generates key internally);
 * SET_KEY_DIRECT reads them. We zero everything outside the keyid and
 * key_ctrl fields so unintended state doesn't leak in.
 */
typedef struct __attribute__((packed)) {
    uint16_t  keyid;                  /* +0   */
    uint32_t  key_ctrl;               /* +2   bits[3:0]=cmd, bits[31:24]=alg */
    uint8_t   reserved_6[58];         /* +6..+63 */
    uint8_t   key_field_1[64];        /* +64..+127  data key */
    uint8_t   key_field_2[64];        /* +128..+191 tweak key */
    uint8_t   reserved_192[64];       /* +192..+255 */
} MktmeKeyProgram;
_Static_assert(sizeof(MktmeKeyProgram) == 256,
               "MKTME_KEY_PROGRAM struct must be exactly 256 bytes");

/* 256-byte-aligned BSS pool — Intel SDM requires 256-byte alignment on
 * the operand. One per BSP-side critical section (g_tme_lock serializes
 * any caller). */
static MktmeKeyProgram g_pconfig_op __attribute__((aligned(256)));

/* ─── PCONFIG instruction wrapper ──────────────────────────────────── */

/* PCONFIG opcode = 0F 01 C5. Inputs: RAX = leaf (0 = MKTME_KEY_PROGRAM),
 * RBX = operand pointer. Output: RAX = status (0 = success).
 *
 * The instruction is CPL-0 only; #UD outside ring 0 or if CPUID.07H.0
 * doesn't advertise it. We gate every caller on g_cpu_caps.has_tme +
 * g_tme.mk_active so this path is only reached on supported silicon.
 *
 * Uses a memory clobber so the compiler can't sink writes to the
 * operand past the instruction. */
static inline uint64_t pconfig_invoke(uint64_t leaf, void *operand) {
    uint64_t result;
    __asm__ volatile (
        ".byte 0x0F, 0x01, 0xC5"        /* PCONFIG */
        : "=a"(result)
        : "a"(leaf), "b"(operand)
        : "memory", "cc"
    );
    return result;
}

/* Program KeyID `keyid` with a CPU-generated random key. Returns
 * TME_PCONFIG_SUCCESS on success; one of the TME_PCONFIG_* error codes
 * on hardware-reported failure. Clobbers g_pconfig_op (caller must
 * hold g_tme_lock or be on the BSP init path). */
static uint64_t pconfig_set_key_random(uint16_t keyid) {
    /* Zero entire operand including key-field bytes. SET_KEY_RANDOM
     * does not READ key_field_1/2 (CPU sources entropy internally) but
     * Intel SDM still requires the operand to be well-formed — any
     * non-reserved field set in violation could yield
     * TME_PCONFIG_INVALID_PROG_CMD. */
    memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));
    g_pconfig_op.keyid    = keyid;
    /* command in bits [3:0]; alg bitmask in bits [31:24]. The alg
     * value matches IA32_TME_ACTIVATE's per-KeyID algorithm field
     * format. We program with the same algorithm firmware activated
     * (mirroring the TME baseline). */
    g_pconfig_op.key_ctrl = TME_CMD_SET_KEY_RANDOM |
                            ((uint32_t)g_tme.pconfig_alg << 24);

    uint64_t rc = pconfig_invoke(0, &g_pconfig_op);

    /* Scrub the operand on the way out — key_field_1/2 were zero on
     * entry, but defensive zero suppresses any compiler-inserted spill
     * + future SET_KEY_DIRECT path reuse from carrying sensitive
     * material. */
    memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));
    return rc;
}

/* ─── Bitmap helpers ───────────────────────────────────────────────── */

static inline bool bitmap_test_local(uint16_t keyid) {
    return (g_tme_bitmap[keyid / 64u] >> (keyid % 64u)) & 1ULL;
}
static inline void bitmap_set_local(uint16_t keyid) {
    g_tme_bitmap[keyid / 64u] |= (1ULL << (keyid % 64u));
}
static inline void bitmap_clear_local(uint16_t keyid) {
    g_tme_bitmap[keyid / 64u] &= ~(1ULL << (keyid % 64u));
}

/* ─── Init ─────────────────────────────────────────────────────────── */

error_t tme_init_bsp(void) {
    if (g_tme_initialized) return OK;  /* idempotent */

    spinlock_init(&g_tme_lock);
    spinlock_init(&g_tme_zero_lock);
    memset(g_tme_bitmap, 0xFF, sizeof(g_tme_bitmap));  /* all in-use by default */
    memset(&g_tme, 0, sizeof(g_tme));

    if (!g_cpu_caps.has_tme) {
        debug_printf("[TME] CPUID.07H.0:ECX[13] = 0 — feature absent, pool dormant\n");
        g_tme_initialized = true;
        return OK;
    }
    if (!g_cpu_caps.has_pconfig) {
        /* Without PCONFIG (CPUID.07H.0:EDX[18]), MKTME_KEY_PROGRAM
         * would #UD. SDM enumerates TME and PCONFIG independently,
         * so a TME-capable CPU is not guaranteed to support PCONFIG.
         * Real-HW corner case; bail before any PCONFIG byte executes. */
        debug_printf("[TME] CPUID.07H.0:EDX[18] = 0 — PCONFIG absent, "
                     "TME-MK pool dormant (TME baseline still active if "
                     "firmware set IA32_TME_ACTIVATE)\n");
        g_tme_initialized = true;
        return OK;
    }

    /* Read IA32_TME_CAPABILITY (0x981) — supported algorithms + max keys. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                     : "c"(VMM_MSR_IA32_TME_CAPABILITY));
    uint64_t cap = ((uint64_t)hi << 32) | lo;

    /* Read IA32_TME_ACTIVATE (0x982) — firmware's TME enable / lock state. */
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                     : "c"(VMM_MSR_IA32_TME_ACTIVATE));
    uint64_t act = ((uint64_t)hi << 32) | lo;

    g_tme.tme_active = (act & VMM_TME_ACT_TME_EN)    != 0;
    g_tme.mk_active  = (act & VMM_TME_ACT_TME_MK_EN) != 0;

    if (!g_tme.tme_active) {
        debug_printf("[TME] IA32_TME_ACTIVATE.TME_EN=0 — firmware did not "
                     "activate TME, pool dormant (act=0x%016lx)\n",
                     (unsigned long)act);
        g_tme_initialized = true;
        return OK;
    }

    g_tme.num_keyid_bits = (uint8_t)((act & VMM_TME_ACT_KEYID_BITS_MASK)
                                     >> VMM_TME_ACT_KEYID_BITS_SHIFT);
    g_tme.reduced_maxphyaddr = vmm_maxphyaddr;
    /* IA32_TME_ACTIVATE bits [7:4] = algorithm selected by firmware. */
    g_tme.activated_alg = (uint8_t)((act >> 4) & 0xFu);
    /* PCONFIG encryption algorithm field uses the same encoding as
     * IA32_TME_ACTIVATE.ALGS_ENABLED bits [7:4]. We mirror it. */
    g_tme.pconfig_alg = g_tme.activated_alg;

    if (!g_tme.mk_active || g_tme.num_keyid_bits == 0) {
        debug_printf("[TME] TME baseline active but MK extension off "
                     "(MK_EN=%d, keyid_bits=%u) — per-KeyID pool dormant; "
                     "platform-key encryption still in effect for ALL RAM\n",
                     (int)g_tme.mk_active, (unsigned)g_tme.num_keyid_bits);
        g_tme_initialized = true;
        return OK;
    }

    /* mk_tme_max_keys from IA32_TME_CAPABILITY bits [50:36]. */
    uint16_t mk_max_keys = (uint16_t)((cap >> 36) & 0x7FFFu);
    if (mk_max_keys == 0) mk_max_keys = (uint16_t)((1u << g_tme.num_keyid_bits) - 1);

    g_tme.max_keyid = mk_max_keys;
    if (g_tme.max_keyid > TME_MAX_KEYIDS - 1)
        g_tme.max_keyid = (uint16_t)(TME_MAX_KEYIDS - 1);

    /* ─── TDX KeyID partitioning (Intel TDX Module Base Architecture
     * Specification §16, "MKTME KeyID Partitioning") ─────────────────
     * When TDX is active, firmware splits the KeyID space into:
     *   "shared" range  (OS use via PCONFIG)  = KeyID 1..NUM_MKTME_KIDS
     *   "private" range (TDX Module per-TD)   = KeyID NUM_MKTME_KIDS+1..MAX
     * IA32_MKTME_KEYID_PARTITIONING (MSR 0x87):
     *   bits [31:0]  = NUM_MKTME_KIDS    (OS-usable count)
     *   bits [63:32] = NUM_TDX_PRIV_KIDS (TDX-reserved count)
     *
     * Reading MSR 0x87 on a non-TDX CPU #GP's, so the gate is the
     * CPUID.21H-derived has_tdx flag (above). On properly-configured
     * TDX firmware, mk_tme_max_keys (IA32_TME_CAPABILITY[50:36]) also
     * reflects the OS-usable count — we clamp to the smaller of
     * the two for defence-in-depth against firmware bugs. */
    if (g_cpu_caps.has_tdx) {
        uint32_t plo, phi;
        __asm__ volatile("rdmsr" : "=a"(plo), "=d"(phi)
                         : "c"(0x87U));
        uint32_t num_mktme_kids = plo;
        uint32_t num_tdx_priv   = phi;
        if (num_mktme_kids > 0 && num_mktme_kids < g_tme.max_keyid) {
            debug_printf("[TME] TDX KeyID partitioning: shared=%u "
                         "tdx_private=%u → clamping pool max %u → %u\n",
                         (unsigned)num_mktme_kids,
                         (unsigned)num_tdx_priv,
                         (unsigned)g_tme.max_keyid,
                         (unsigned)num_mktme_kids);
            g_tme.max_keyid = (uint16_t)num_mktme_kids;
        } else {
            debug_printf("[TME] TDX present but partitioning MSR shows "
                         "shared=%u (>= pool max %u) — no clamp needed\n",
                         (unsigned)num_mktme_kids,
                         (unsigned)g_tme.max_keyid);
        }
    }

    /* Widen vmm_pte_addr_mask_with_keyid to cover KeyID bits. The
     * narrow vmm_pte_addr_mask strips at reduced MAXPHYADDR; for
     * KeyID-bearing PTEs (vmm_make_pte_with_keyid) we need to preserve
     * bits up to raw MAXPHYADDR = reduced + num_keyid_bits. The PTE
     * phys field is capped at bit 51 architecturally, so clamp to
     * 0x000FFFFFFFFFF000 if the computed bound would exceed it. */
    {
        uint8_t raw_maxphyaddr = (uint8_t)(g_tme.reduced_maxphyaddr +
                                            g_tme.num_keyid_bits);
        if (raw_maxphyaddr >= 52) {
            vmm_pte_addr_mask_with_keyid = 0x000FFFFFFFFFF000ULL;
        } else {
            uint64_t bound = (1ULL << raw_maxphyaddr);
            vmm_pte_addr_mask_with_keyid = (bound - 1ULL) & 0xFFFFFFFFFFFFF000ULL;
        }
        debug_printf("[TME] Widened PTE addr mask: 0x%016lx → 0x%016lx "
                     "(covers raw MAXPHYADDR=%u)\n",
                     (unsigned long)vmm_pte_addr_mask,
                     (unsigned long)vmm_pte_addr_mask_with_keyid,
                     (unsigned)raw_maxphyaddr);
    }

    debug_printf("[TME] cap=0x%016lx act=0x%016lx tme_en=1 mk_en=1 "
                 "keyid_bits=%u max_keyid=%u alg=%u "
                 "reduced_maxphyaddr=%u → KeyID at PA bits [%u:%u]\n",
                 (unsigned long)cap, (unsigned long)act,
                 (unsigned)g_tme.num_keyid_bits,
                 (unsigned)g_tme.max_keyid,
                 (unsigned)g_tme.activated_alg,
                 (unsigned)g_tme.reduced_maxphyaddr,
                 (unsigned)(g_tme.reduced_maxphyaddr + g_tme.num_keyid_bits - 1),
                 (unsigned)g_tme.reduced_maxphyaddr);

    /* Program KeyIDs 1..max_keyid with SET_KEY_RANDOM. KeyID 0 stays
     * reserved (platform default). */
    g_tme.pool_size = g_tme.max_keyid;  /* slots 1..max_keyid */
    g_tme.pool_programmed = 0;

    for (uint16_t k = 1; k <= g_tme.max_keyid; k++) {
        uint64_t rc = pconfig_set_key_random(k);
        if (rc == TME_PCONFIG_SUCCESS) {
            bitmap_clear_local(k);  /* mark free */
            g_tme.pool_programmed++;
        } else {
            debug_printf("[TME] KeyID %u PCONFIG SET_KEY_RANDOM failed: "
                         "rc=%lu (slot dropped from pool)\n",
                         (unsigned)k, (unsigned long)rc);
            /* leave bitmap bit set → never allocatable */
        }
    }

    /* Pre-resolve Touch tag IDs for fast publish. Use the reserved
     * tme:* namespace seeded in memtag.c (Phase 2J). TouchTagIntern
     * resolves through the unified tag registry. */
    g_tme_tag_pool_ready   = TouchTagIntern("tme:pool:ready");
    g_tme_tag_rekey_failed = TouchTagIntern("tme:keyid:rekey:failed");

    /* Publish pool-ready Touch event with the programmed count in the
     * payload so subscribers can see capacity. */
    if (g_tme_tag_pool_ready != TOUCH_TAG_INVALID) {
        uint32_t payload = g_tme.pool_programmed;
        TouchPublishId(g_tme_tag_pool_ready, &payload, sizeof(payload),
                       0 /* source_pid: kernel */, 0 /* flags */);
    }

    debug_printf("[TME] Pool initialized: %u/%u KeyIDs programmed\n",
                 (unsigned)g_tme.pool_programmed,
                 (unsigned)g_tme.pool_size);

    g_tme_initialized = true;
    return OK;
}

/* ─── Allocator ────────────────────────────────────────────────────── */

error_t tme_keyid_alloc(uint16_t *out_keyid) {
    if (!g_tme_initialized) return ERR_NOT_INITIALIZED;
    if (!out_keyid)         return ERR_INVALID_ARGUMENT;
    if (!g_tme.mk_active)   return ERR_UNSUPPORTED;

    spin_lock(&g_tme_lock);

    /* Linear scan over bitmap; with N ≤ 256 this is ≤ 4 qword tests. */
    for (uint16_t k = 1; k <= g_tme.max_keyid; k++) {
        if (!bitmap_test_local(k)) {
            bitmap_set_local(k);
            g_tme.in_use++;
            spin_unlock(&g_tme_lock);
            *out_keyid = k;
            return OK;
        }
    }

    spin_unlock(&g_tme_lock);
    return ERR_NO_MEMORY;
}

error_t tme_keyid_free(uint16_t keyid) {
    if (!g_tme_initialized) return ERR_NOT_INITIALIZED;
    if (!g_tme.mk_active)   return ERR_UNSUPPORTED;
    if (keyid == 0 || keyid > g_tme.max_keyid) return ERR_INVALID_ARGUMENT;

    /* Re-key the slot under the lock so no concurrent allocator can
     * grab the slot mid-rekey and observe a transient state. PCONFIG
     * itself takes microseconds on real silicon — short critical
     * section. */
    spin_lock(&g_tme_lock);

    if (!bitmap_test_local(keyid)) {
        spin_unlock(&g_tme_lock);
        return ERR_INVALID_ARGUMENT;  /* double-free */
    }

    uint64_t rc = pconfig_set_key_random(keyid);
    if (rc != TME_PCONFIG_SUCCESS) {
        /* Slot is now in an undefined state — leave it marked in-use
         * forever so no future allocation can reach it. Better to
         * lose one slot than risk programming-failure plus key reuse. */
        spin_unlock(&g_tme_lock);
        debug_printf("[TME] KeyID %u re-key failed (rc=%lu); slot lost\n",
                     (unsigned)keyid, (unsigned long)rc);
        if (g_tme_tag_rekey_failed != TOUCH_TAG_INVALID) {
            struct { uint16_t keyid; uint16_t pad; uint64_t pconfig_rc; }
                payload = { keyid, 0, rc };
            TouchPublishId(g_tme_tag_rekey_failed, &payload, sizeof(payload),
                           0 /* source_pid: kernel */, 0 /* flags */);
        }
        return ERR_IO;
    }

    bitmap_clear_local(keyid);
    if (g_tme.in_use > 0) g_tme.in_use--;
    spin_unlock(&g_tme_lock);
    return OK;
}

/* ─── Encoders ─────────────────────────────────────────────────────── */

uint64_t tme_phys_with_keyid(uint64_t phys, uint16_t keyid) {
    if (!g_tme.mk_active) return phys;
    return vmm_phys_with_keyid(phys, (uint8_t)keyid,
                               g_tme.num_keyid_bits,
                               g_tme.reduced_maxphyaddr);
}

uint64_t tme_phys_strip_keyid(uint64_t phys_with_keyid) {
    if (!g_tme.mk_active) return phys_with_keyid;
    if (g_tme.num_keyid_bits == 0 || g_tme.num_keyid_bits >= 64) return phys_with_keyid;
    uint64_t mask = ((1ULL << g_tme.num_keyid_bits) - 1ULL)
                    << g_tme.reduced_maxphyaddr;
    return phys_with_keyid & ~mask;
}

/* ─── Zero-fill with KeyID-bearing kernel mapping ──────────────────── */

/* Reserved kernel VA slot for TME zero-fill. Placed in the unused
 * 0xFFFF_9000_0000_0000 region between VMM_KERNEL_BASE and
 * PULL_MAP_BASE. 2 MiB span suffices for the largest single mapping
 * (huge-page case). Serialized by g_tme_zero_lock — zero-fill is a
 * cold path, contention is acceptable. */
#define TME_ZERO_VA   0xFFFF900000000000ULL
#define TME_ZERO_SIZE (2ULL * 1024ULL * 1024ULL)
/* g_tme_zero_lock declared at top with the other static state. */

error_t tme_zero_pages_with_keyid(uintptr_t phys_base, size_t pages,
                                   uint16_t keyid, bool huge_2m)
{
    if (!g_tme.mk_active) return ERR_UNSUPPORTED;
    if (pages == 0) return OK;
    if (keyid == 0 || keyid > g_tme.max_keyid) return ERR_INVALID_ARGUMENT;

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx) return ERR_NOT_INITIALIZED;

    /* Single-page granularity: 4 KiB or 2 MiB. We iterate pages or
     * huge-pages and map/zero/unmap one at a time. */
    const size_t step_pages = huge_2m ? 512u : 1u;
    const size_t step_bytes = huge_2m ? (2u * 1024u * 1024u) : 4096u;

    spin_lock(&g_tme_zero_lock);

    for (size_t i = 0; i < pages; i += step_pages) {
        uintptr_t phys = phys_base + (uintptr_t)i * 4096u;
        uintptr_t pa_keyid = tme_phys_with_keyid(phys, keyid);

        bool mapped;
        if (huge_2m) {
            mapped = vmm_map_huge_2m_with_keyid(kctx, TME_ZERO_VA,
                                                 pa_keyid, VMM_FLAGS_KERNEL_RW);
        } else {
            vmm_map_result_t r = vmm_map_page_with_keyid(kctx,
                                                         TME_ZERO_VA,
                                                         pa_keyid,
                                                         VMM_FLAGS_KERNEL_RW);
            mapped = r.success;
        }
        if (!mapped) {
            spin_unlock(&g_tme_zero_lock);
            return ERR_NO_MEMORY;
        }

        memset((void *)TME_ZERO_VA, 0, step_bytes);

        if (huge_2m) {
            vmm_unmap_huge_2m(kctx, TME_ZERO_VA);
        } else {
            vmm_unmap_page(kctx, TME_ZERO_VA);
        }
    }

    spin_unlock(&g_tme_zero_lock);
    return OK;
}

/* ─── DMA safety gate ──────────────────────────────────────────────── */

bool tme_is_safe_for_dma(uintptr_t phys_with_keyid) {
    /* If TME-MK is inactive every phys carries an implicit KeyID 0
     * (platform default); DMA sees baseline-encrypted DRAM which is
     * transparent to the device under TME-only operation. */
    if (!g_tme.mk_active) return true;

    if (g_tme.num_keyid_bits == 0 || g_tme.num_keyid_bits >= 64) return true;
    uint64_t mask = ((1ULL << g_tme.num_keyid_bits) - 1ULL)
                    << g_tme.reduced_maxphyaddr;
    uint64_t keyid_bits = phys_with_keyid & mask;

    /* KeyID 0 — platform default key. Safe for DMA on TME-only hosts;
     * UNSAFE on TDX-partitioned hosts (private KeyID 0). BoxOS doesn't
     * yet detect TDX explicitly; assume non-TDX for now. */
    return keyid_bits == 0;
}

/* ─── Dump ─────────────────────────────────────────────────────────── */

void tme_dump_state(void) {
    debug_printf("[TME] state: initialized=%d tme_active=%d mk_active=%d "
                 "num_keyid_bits=%u max_keyid=%u alg=%u "
                 "reduced_maxphyaddr=%u pool=%u programmed=%u in_use=%u\n",
                 (int)g_tme_initialized,
                 (int)g_tme.tme_active,
                 (int)g_tme.mk_active,
                 (unsigned)g_tme.num_keyid_bits,
                 (unsigned)g_tme.max_keyid,
                 (unsigned)g_tme.activated_alg,
                 (unsigned)g_tme.reduced_maxphyaddr,
                 (unsigned)g_tme.pool_size,
                 (unsigned)g_tme.pool_programmed,
                 (unsigned)g_tme.in_use);
}
