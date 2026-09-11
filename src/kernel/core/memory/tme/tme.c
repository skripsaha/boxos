
#include "tme.h"
#include "vmm.h"
#include "memtag.h"
#include "klib.h"
#include "touch.h"
#include "logbook.h"
#include "debug.h"
#include "cpuid.h"


TmeState g_tme = { 0 };


#define TME_BITMAP_WORDS  (TME_MAX_KEYIDS / 64u)
static uint64_t   g_tme_bitmap[TME_BITMAP_WORDS];
static spinlock_t g_tme_lock;
static spinlock_t g_tme_zero_lock;
static bool       g_tme_initialized = false;

static TouchTag   g_tme_tag_pool_ready    = TOUCH_TAG_INVALID;
static TouchTag   g_tme_tag_rekey_failed  = TOUCH_TAG_INVALID;


typedef struct __attribute__((packed)) {
    uint16_t  keyid;
    uint32_t  key_ctrl;
    uint8_t   reserved_6[58];
    uint8_t   key_field_1[64];
    uint8_t   key_field_2[64];
    uint8_t   reserved_192[64];
} MktmeKeyProgram;
_Static_assert(sizeof(MktmeKeyProgram) == 256,
               "MKTME_KEY_PROGRAM struct must be exactly 256 bytes");

static MktmeKeyProgram g_pconfig_op __attribute__((aligned(256)));


static inline uint64_t pconfig_invoke(uint64_t leaf, void *operand) {
    uint64_t result;
    __asm__ volatile (
        ".byte 0x0F, 0x01, 0xC5"
        : "=a"(result)
        : "a"(leaf), "b"(operand)
        : "memory", "cc"
    );
    return result;
}

static uint64_t pconfig_set_key_random(uint16_t keyid) {
    memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));
    g_pconfig_op.keyid    = keyid;
    g_pconfig_op.key_ctrl = TME_CMD_SET_KEY_RANDOM |
                            ((uint32_t)g_tme.pconfig_alg << 24);

    uint64_t rc = pconfig_invoke(0, &g_pconfig_op);

    memset(&g_pconfig_op, 0, sizeof(g_pconfig_op));
    return rc;
}


static inline bool bitmap_test_local(uint16_t keyid) {
    return (g_tme_bitmap[keyid / 64u] >> (keyid % 64u)) & 1ULL;
}
static inline void bitmap_set_local(uint16_t keyid) {
    g_tme_bitmap[keyid / 64u] |= (1ULL << (keyid % 64u));
}
static inline void bitmap_clear_local(uint16_t keyid) {
    g_tme_bitmap[keyid / 64u] &= ~(1ULL << (keyid % 64u));
}


error_t tme_init_bsp(void) {
    if (g_tme_initialized) return OK;

    spinlock_init(&g_tme_lock);
    spinlock_init(&g_tme_zero_lock);
    memset(g_tme_bitmap, 0xFF, sizeof(g_tme_bitmap));
    memset(&g_tme, 0, sizeof(g_tme));

    if (!g_cpu_caps.has_tme) {
        debug_printf("[TME] CPUID.07H.0:ECX[13] = 0 — feature absent, pool dormant\n");
        g_tme_initialized = true;
        return OK;
    }
    if (!g_cpu_caps.has_pconfig) {
        debug_printf("[TME] CPUID.07H.0:EDX[18] = 0 — PCONFIG absent, "
                     "TME-MK pool dormant (TME baseline still active if "
                     "firmware set IA32_TME_ACTIVATE)\n");
        g_tme_initialized = true;
        return OK;
    }

    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                     : "c"(VMM_MSR_IA32_TME_CAPABILITY));
    uint64_t cap = ((uint64_t)hi << 32) | lo;

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
    g_tme.activated_alg = (uint8_t)((act >> 4) & 0xFu);
    g_tme.pconfig_alg = g_tme.activated_alg;

    if (!g_tme.mk_active || g_tme.num_keyid_bits == 0) {
        debug_printf("[TME] TME baseline active but MK extension off "
                     "(MK_EN=%d, keyid_bits=%u) — per-KeyID pool dormant; "
                     "platform-key encryption still in effect for ALL RAM\n",
                     (int)g_tme.mk_active, (unsigned)g_tme.num_keyid_bits);
        g_tme_initialized = true;
        return OK;
    }

    uint16_t mk_max_keys = (uint16_t)((cap >> 36) & 0x7FFFu);
    if (mk_max_keys == 0) mk_max_keys = (uint16_t)((1u << g_tme.num_keyid_bits) - 1);

    g_tme.max_keyid = mk_max_keys;
    if (g_tme.max_keyid > TME_MAX_KEYIDS - 1)
        g_tme.max_keyid = (uint16_t)(TME_MAX_KEYIDS - 1);

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

    g_tme.pool_size = g_tme.max_keyid;
    g_tme.pool_programmed = 0;

    for (uint16_t k = 1; k <= g_tme.max_keyid; k++) {
        uint64_t rc = pconfig_set_key_random(k);
        if (rc == TME_PCONFIG_SUCCESS) {
            bitmap_clear_local(k);
            g_tme.pool_programmed++;
        } else {
            debug_printf("[TME] KeyID %u PCONFIG SET_KEY_RANDOM failed: "
                         "rc=%lu (slot dropped from pool)\n",
                         (unsigned)k, (unsigned long)rc);
        }
    }

    g_tme_tag_pool_ready   = TouchLogbookIntern("tme:pool:ready");
    g_tme_tag_rekey_failed = TouchLogbookIntern("tme:keyid:rekey:failed");

    if (g_tme_tag_pool_ready != TOUCH_TAG_INVALID) {
        uint32_t payload = g_tme.pool_programmed;
        TouchPublishId(g_tme_tag_pool_ready, &payload, sizeof(payload),
                       0 , 0 );
    }

    debug_printf("[TME] Pool initialized: %u/%u KeyIDs programmed\n",
                 (unsigned)g_tme.pool_programmed,
                 (unsigned)g_tme.pool_size);

    g_tme_initialized = true;
    return OK;
}


error_t tme_keyid_alloc(uint16_t *out_keyid) {
    if (!g_tme_initialized) return ERR_NOT_INITIALIZED;
    if (!out_keyid)         return ERR_INVALID_ARGUMENT;
    if (!g_tme.mk_active)   return ERR_UNSUPPORTED;

    spin_lock(&g_tme_lock);

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

    spin_lock(&g_tme_lock);

    if (!bitmap_test_local(keyid)) {
        spin_unlock(&g_tme_lock);
        return ERR_INVALID_ARGUMENT;
    }

    uint64_t rc = pconfig_set_key_random(keyid);
    if (rc != TME_PCONFIG_SUCCESS) {
        spin_unlock(&g_tme_lock);
        debug_printf("[TME] KeyID %u re-key failed (rc=%lu); slot lost\n",
                     (unsigned)keyid, (unsigned long)rc);
        if (g_tme_tag_rekey_failed != TOUCH_TAG_INVALID) {
            struct { uint16_t keyid; uint16_t pad; uint64_t pconfig_rc; }
                payload = { keyid, 0, rc };
            TouchPublishId(g_tme_tag_rekey_failed, &payload, sizeof(payload),
                           0 , 0 );
        }
        return ERR_IO;
    }

    bitmap_clear_local(keyid);
    if (g_tme.in_use > 0) g_tme.in_use--;
    spin_unlock(&g_tme_lock);
    return OK;
}


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


#define TME_ZERO_VA   0xFFFF900000000000ULL
#define TME_ZERO_SIZE (2ULL * 1024ULL * 1024ULL)

error_t tme_zero_pages_with_keyid(uintptr_t phys_base, size_t pages,
                                   uint16_t keyid, bool huge_2m)
{
    if (!g_tme.mk_active) return ERR_UNSUPPORTED;
    if (pages == 0) return OK;
    if (keyid == 0 || keyid > g_tme.max_keyid) return ERR_INVALID_ARGUMENT;

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx) return ERR_NOT_INITIALIZED;

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


bool tme_is_safe_for_dma(uintptr_t phys_with_keyid) {
    if (!g_tme.mk_active) return true;

    if (g_tme.num_keyid_bits == 0 || g_tme.num_keyid_bits >= 64) return true;
    uint64_t mask = ((1ULL << g_tme.num_keyid_bits) - 1ULL)
                    << g_tme.reduced_maxphyaddr;
    uint64_t keyid_bits = phys_with_keyid & mask;

    return keyid_bits == 0;
}


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