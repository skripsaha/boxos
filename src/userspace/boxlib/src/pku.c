#include "box/timeouts.h"
#include "box/pku.h"
#include "box/cpu.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"  /* SYSTEM_OP_MEMTAG_APPLY_PKEY — single source */

/* PKU detection comes from the vDSO-shaped cpu_caps page (mapped at
 * CABIN_CPU_CAPS_ADDR by the kernel for every cabin). cpu_has_pku()
 * reads a single byte — no syscall, no inline CPUID, no AP-divergence
 * surprises (the kernel refreshes that byte after every AP intersect). */

uint32_t pku_read_pkru(void) {
    if (!cpu_has_pku()) return 0;
    uint32_t eax;
    uint32_t edx_dummy;
    /* RDPKRU = 0F 01 EE — ECX must be 0. EDX cleared on exit. */
    __asm__ volatile(".byte 0x0F, 0x01, 0xEE"
                     : "=a"(eax), "=d"(edx_dummy)
                     : "c"(0));
    return eax;
}

void pku_write_pkru(uint32_t value) {
    if (!cpu_has_pku()) return;
    /* WRPKRU = 0F 01 EF — EAX=value, ECX=0, EDX=0 (else #GP). */
    __asm__ volatile(".byte 0x0F, 0x01, 0xEF"
                     :
                     : "a"(value), "c"(0), "d"(0)
                     : "memory");
}

/* ─── Per-key rights ──────────────────────────────────────────────── */

int pku_set_rights(uint8_t pkey, int ad, int wd) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    if (!cpu_has_pku()) return -ERR_NOT_IMPLEMENTED;
    uint32_t pkru = pku_read_pkru();
    uint32_t shift = (uint32_t)pkey * 2u;
    uint32_t mask  = 0x3u << shift;
    uint32_t bits  = (((ad ? 1u : 0u)) | ((wd ? 2u : 0u))) << shift;
    pkru = (pkru & ~mask) | bits;
    pku_write_pkru(pkru);
    return 0;
}

int pku_get_rights(uint8_t pkey, int *out_ad, int *out_wd) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    if (!cpu_has_pku()) return -ERR_NOT_IMPLEMENTED;
    uint32_t pkru = pku_read_pkru();
    uint32_t shift = (uint32_t)pkey * 2u;
    if (out_ad) *out_ad = (int)((pkru >> shift) & 1u);
    if (out_wd) *out_wd = (int)((pkru >> (shift + 1u)) & 1u);
    return 0;
}

/* ─── Region stamping via MemTag syscall ──────────────────────────── */

int pku_apply_region(uint32_t region_id, uint8_t pkey) {
    if (pkey >= PKU_MAX_KEYS) return -ERR_INVALID_ARGS;
    uint8_t params[sizeof(uint32_t) + sizeof(uint8_t)];
    memcpy(params, &region_id, sizeof(uint32_t));
    params[sizeof(uint32_t)] = pkey;
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_MEMTAG_APPLY_PKEY,
                   params, sizeof(params),
                   0, 0,
                   0, 0, 0,
                   BOX_ANSWER_GUARANTEED, 0);
}
