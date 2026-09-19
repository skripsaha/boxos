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
#include "logbook.h"
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

/* Intel SDM Vol 2D, PCONFIG leaf MKTME_KEY_PROGRAM, Table 4-15
 * "MKTME_KEY_PROGRAM_STRUCT" (= Intel 336907 §6.2.1.1 Table 6-4):
 *   off    size  field
 *     0    2     KEYID
 *     2    4     KEYID_CTRL  (bits 7:0 COMMAND, bits 23:8 ENC_ALG,
 *                             bits 31:24 RSVD — must be zero)
 *     6   58     Ignored
 *    64   64     KEY_FIELD_1 (data key or entropy)
 *   128   64     KEY_FIELD_2 (tweak key or entropy)
 *   --- 192 bytes are loaded; the operand must be 256-byte ALIGNED ----
 *
 * The architectural operand is 192 bytes ("Load TMP_KEY_PROGRAM_STRUCT
 * from 192 bytes at linear address DS:RBX"); it is the ALIGNMENT that is
 * 256. The trailing 64 bytes below are ours, not the CPU's — they cost
 * nothing and keep the object a whole alignment granule.
 *
 * Bytes 63:6 are Ignored, not reserved: the manual retracted the older
 * claim that a non-zero value there would #GP. We zero them anyway, so
 * nothing stale reaches the instruction.
 *
 * SET_KEY_RANDOM ignores KEY_FIELD_1/2 (CPU generates key internally);
 * SET_KEY_DIRECT reads them.
 */
typedef struct __attribute__((packed)) {
    uint16_t  keyid;                  /* +0   */
    uint32_t  key_ctrl;               /* +2   bits 7:0 cmd, 23:8 enc_alg */
    uint8_t   ignored_6[58];          /* +6..+63 */
    uint8_t   key_field_1[64];        /* +64..+127  data key */
    uint8_t   key_field_2[64];        /* +128..+191 tweak key */
    uint8_t   tail_pad[64];           /* +192..+255 BoxOS padding */
} MktmeKeyProgram;
_Static_assert(sizeof(MktmeKeyProgram) == 256,
               "MKTME_KEY_PROGRAM operand: 192 architectural bytes plus "
               "BoxOS padding to one 256-byte alignment granule");

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

/* Both failure reasons PCONFIG can REPORT are ones it asks software to
 * take again, and in both the key table is left untouched:
 *
 *   DEVICE_BUSY (5) — "An execution of the leaf function may fail to
 *   acquire the lock if it is already in use... indicating that software
 *   must retry"; "the key table is not updated, and software should
 *   retry execution of PCONFIG". The lock is held for the length of one
 *   PCONFIG, and the holder can be another core or another package's
 *   agent — g_tme_lock is a BoxOS lock and does not reach it.
 *
 *   ENTROPY_ERROR (2) — "If there was insufficient entropy for the
 *   random-number generator". The on-die DRNG reseeds on its own; the
 *   shortage is momentary.
 *
 * So the answer to either is to ask again, not to lose the slot. There
 * is nothing here to time and nothing to wait for: we hand the core back
 * with PAUSE and re-issue. The attempt budget follows Intel's own DRNG
 * guidance for the same class of transient refusal (retry ten times,
 * separated by PAUSE); a refusal that survives that is not transient,
 * and the caller has to hear about it rather than spin forever. */
#define TME_PCONFIG_RETRIES  10u

static inline bool pconfig_rc_is_retryable(uint64_t rc) {
    return rc == TME_PCONFIG_DEVICE_BUSY || rc == TME_PCONFIG_ENTROPY_ERROR;
}

/* Program KeyID `keyid` with a CPU-generated random key. Returns
 * TME_PCONFIG_SUCCESS on success; one of the TME_PCONFIG_* error codes
 * on hardware-reported failure, after exhausting the retry budget for
 * the two retryable ones. Clobbers g_pconfig_op (caller must hold
 * g_tme_lock or be on the BSP init path). */
static uint64_t pconfig_set_key_random(uint16_t keyid) {
    /* ENC_ALG must set EXACTLY ONE bit, and that bit must also be set in
     * IA32_TME_ACTIVATE[63:48], or PCONFIG raises #GP(0) — not a status
     * in EAX. Refuse here rather than execute an instruction that is
     * guaranteed to fault at CPL 0. tme_init_bsp already declines to arm
     * the pool in this state, so this is the second line of defence. */
    if (g_tme.pconfig_alg == 0) return TME_PCONFIG_INVALID_ENC_ALG;

    uint64_t rc = TME_PCONFIG_SUCCESS;

    for (unsigned attempt = 0; ; attempt++) {
        /* Rebuild the operand every attempt: the scrub at the bottom of
         * the previous one wiped it, and PCONFIG may have written to it.
         * Zeroing covers the key-field and the bytes the manual marks
         * Ignored, so nothing stale reaches the instruction.
         * SET_KEY_RANDOM does not READ key_field_1/2 — the CPU sources
         * entropy internally. */
        memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));
        g_pconfig_op.keyid    = keyid;
        /* KEYID_CTRL: COMMAND in bits 7:0, ENC_ALG in bits 23:8, bits
         * 31:24 RSVD and left zero. ENC_ALG is the 16-bit one-hot
         * selection drawn from the IA32_TME_ACTIVATE[63:48] algorithm
         * bitmap — not the 4-bit TME_POLICY nibble, which encodes the
         * TME BASELINE algorithm and means nothing in this field. */
        g_pconfig_op.key_ctrl = (uint32_t)TME_CMD_SET_KEY_RANDOM |
                                ((uint32_t)g_tme.pconfig_alg << 8);

        rc = pconfig_invoke(0, &g_pconfig_op);

        /* Scrub the operand on the way out — key_field_1/2 were zero on
         * entry, but defensive zero suppresses any compiler-inserted
         * spill + future SET_KEY_DIRECT path reuse from carrying
         * sensitive material. */
        memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));

        if (!pconfig_rc_is_retryable(rc)) break;
        if (attempt >= TME_PCONFIG_RETRIES) {
            debug_printf("[TME] KeyID %u: PCONFIG still rc=%lu after %u "
                         "retries — treating as permanent\n",
                         (unsigned)keyid, (unsigned long)rc,
                         (unsigned)TME_PCONFIG_RETRIES);
            break;
        }
        cpu_pause();
    }

    return rc;
}

/* ─── Encryption-algorithm selection ───────────────────────────────── */

/* Pick the ENC_ALG value for KEYID_CTRL bits 23:8 out of the
 * IA32_TME_ACTIVATE[63:48] bitmap of algorithms firmware allows.
 *
 * The 16-bit ENC_ALG field lines up bit-for-bit with that window: its
 * bit 0 is MSR bit 48 (AES-XTS-128), bit 1 is MSR bit 49 (AES-XTS-128
 * with integrity), bit 2 is MSR bit 50 (AES-XTS-256); the rest are
 * reserved. PCONFIG requires EXACTLY ONE bit set and that bit present
 * in the bitmap, so we hand back the lowest allowed one. Returns 0 when
 * firmware allowed nothing — the caller must then not issue PCONFIG. */
static uint16_t tme_pick_enc_alg(uint64_t act) {
    uint32_t algs = (uint32_t)((act & VMM_TME_ACT_ALGS_MASK)
                               >> VMM_TME_ACT_ALGS_SHIFT);
    return (uint16_t)(algs & (~algs + 1u));   /* lowest set bit, 0 if none */
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

    g_tme.tme_active = (act & VMM_TME_ACT_TME_EN) != 0;

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
    /* IA32_TME_ACTIVATE[7:4] TME_POLICY — the TME BASELINE algorithm
     * firmware chose. Reported for diagnostics; it is not what PCONFIG
     * wants. */
    g_tme.activated_alg = (uint8_t)((act >> 4) & 0xFu);
    /* PCONFIG KEYID_CTRL.ENC_ALG is a 16-bit one-hot selection out of
     * IA32_TME_ACTIVATE[63:48] (bit 48 AES-XTS-128, bit 49 AES-XTS-128
     * with integrity, bit 50 AES-XTS-256). Take the lowest algorithm
     * firmware allows. */
    g_tme.pconfig_alg = tme_pick_enc_alg(act);

    /* TME-MK is active iff LOCK=1, TME_EN=1 and MK_TME_KEYID_BITS != 0 —
     * exactly what PCONFIG checks before it will program anything. There
     * is no MK enable bit; bits 47:36 are Reserved. */
    g_tme.mk_active = vmm_tme_act_mk_active(act);

    if (!g_tme.mk_active) {
        debug_printf("[TME] TME baseline active but MK extension not usable "
                     "(lock=%d TME_EN=1 keyid_bits=%u) — per-KeyID pool "
                     "dormant; platform-key encryption still in effect for "
                     "ALL RAM\n",
                     (int)((act & VMM_TME_ACT_LOCK) != 0),
                     (unsigned)g_tme.num_keyid_bits);
        g_tme_initialized = true;
        return OK;
    }

    if (g_tme.pconfig_alg == 0) {
        /* Firmware activated TME-MK but allowed no algorithm. Every
         * PCONFIG would #GP(0) at CPL 0 on the ENC_ALG check, so the pool
         * stays dormant. Visible on the board: this is a firmware state
         * an operator has to see, not a debug detail. */
        kprintf("[TME] MK active but IA32_TME_ACTIVATE[63:48]=0 — firmware "
                "allows no encryption algorithm; KeyID pool stays dormant "
                "(act=0x%016lx)\n", (unsigned long)act);
        g_tme.mk_active = false;
        g_tme_initialized = true;
        return OK;
    }

    /* More than one algorithm allowed means the cipher every KeyID gets
     * was a CHOICE, and the lowest allowed bit is not the strongest one
     * (bit 48 AES-XTS-128 sorts below bit 50 AES-XTS-256). The policy
     * itself is the owner's to set; what must not happen is it being
     * made invisibly. debug_printf compiles to nothing, so this one goes
     * through kprintf — an operator on the board has to be able to read
     * which cipher the pool was keyed with. */
    {
        uint16_t allowed = (uint16_t)((act & VMM_TME_ACT_ALGS_MASK)
                                      >> VMM_TME_ACT_ALGS_SHIFT);
        if ((allowed & (uint16_t)(allowed - 1u)) != 0u)
            kprintf("[TME] firmware allows algorithms 0x%04x; keying the "
                    "pool with 0x%04x (lowest allowed)\n",
                    (unsigned)allowed, (unsigned)g_tme.pconfig_alg);
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

    debug_printf("[TME] cap=0x%016lx act=0x%016lx tme_en=1 mk_active=1 "
                 "keyid_bits=%u max_keyid=%u policy=%u enc_alg=0x%04x "
                 "reduced_maxphyaddr=%u → KeyID at PA bits [%u:%u]\n",
                 (unsigned long)cap, (unsigned long)act,
                 (unsigned)g_tme.num_keyid_bits,
                 (unsigned)g_tme.max_keyid,
                 (unsigned)g_tme.activated_alg,
                 (unsigned)g_tme.pconfig_alg,
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
            /* The wrapper already re-issued on the two retryable
             * reasons, so this failure is the permanent kind. */
            debug_printf("[TME] KeyID %u PCONFIG SET_KEY_RANDOM failed: "
                         "rc=%lu (slot dropped from pool)\n",
                         (unsigned)k, (unsigned long)rc);
            /* leave bitmap bit set → never allocatable */
        }
    }

    /* Pre-resolve Touch tag IDs for fast publish. Use the reserved
     * tme:* namespace seeded in memtag.c (Phase 2J). TouchLogbookIntern
     * resolves through the unified tag registry. */
    g_tme_tag_pool_ready   = TouchLogbookIntern("tme:pool:ready");
    g_tme_tag_rekey_failed = TouchLogbookIntern("tme:keyid:rekey:failed");

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

    /* A pool that came up short is a pool that will refuse encrypted Bays
     * later, for a reason that happened here. Said once, out loud: the
     * per-slot lines above are debug_printf and are not there on a board,
     * and a pool of zero is the whole feature gone without a word. */
    if (g_tme.pool_programmed < g_tme.pool_size)
        kprintf("[TME] %u of %u KeyIDs would not take a key; the pool holds "
                "%u\n",
                (unsigned)(g_tme.pool_size - g_tme.pool_programmed),
                (unsigned)g_tme.pool_size,
                (unsigned)g_tme.pool_programmed);

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
        /* The wrapper already re-issued on DEVICE_BUSY and ENTROPY_ERROR
         * — the two reasons the manual says to retry, and the two a
         * contending key-table holder produces. Reaching here means the
         * refusal outlived the budget.
         *
         * What is left in the slot is not undefined: on both codes the
         * manual leaves the key table alone ("the key table is not
         * updated" for DEVICE_BUSY; ENTROPY_ERROR releases the platform
         * lock and exits before the table is written). The slot still
         * holds the key of the Bay that has just gone. THAT is why it
         * must not go back in the pool: handing it to the next caller
         * would hand them a key that decrypts the dead Bay's ciphertext.
         * Leave the bit set so no allocation can reach it.
         *
         * Both counters move, because both facts changed. The
         * reservation is over — the holder gave the KeyID back and this
         * call is what ends it — so in_use comes down; and the slot is
         * no longer one the pool can hand out, so pool_programmed comes
         * down with it. Leave either standing and tme_dump_state drifts
         * by one per lost slot: in_use stops meaning "held right now",
         * or programmed - in_use stops meaning "free". */
        if (g_tme.in_use > 0) g_tme.in_use--;
        if (g_tme.pool_programmed > 0) g_tme.pool_programmed--;
        spin_unlock(&g_tme_lock);
        /* A KeyID that will never be handed out again is a capability
         * this machine has permanently lost. An operator on the board
         * has to be able to read that; debug_printf compiles to nothing
         * in a shipping build. */
        kprintf("[TME] KeyID %u re-key failed (rc=%lu); slot lost for the "
                "life of this boot\n",
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
                 "num_keyid_bits=%u max_keyid=%u policy=%u enc_alg=0x%04x "
                 "reduced_maxphyaddr=%u pool=%u programmed=%u in_use=%u\n",
                 (int)g_tme_initialized,
                 (int)g_tme.tme_active,
                 (int)g_tme.mk_active,
                 (unsigned)g_tme.num_keyid_bits,
                 (unsigned)g_tme.max_keyid,
                 (unsigned)g_tme.activated_alg,
                 (unsigned)g_tme.pconfig_alg,
                 (unsigned)g_tme.reduced_maxphyaddr,
                 (unsigned)g_tme.pool_size,
                 (unsigned)g_tme.pool_programmed,
                 (unsigned)g_tme.in_use);
}
