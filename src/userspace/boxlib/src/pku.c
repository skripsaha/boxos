#include "box/timeouts.h"
#include "box/pku.h"
#include "box/cpu.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"


uint32_t pku_read_pkru(void) {
    if (!cpu_has_pku()) return 0;
    uint32_t eax;
    uint32_t edx_dummy;
    __asm__ volatile(".byte 0x0F, 0x01, 0xEE"
                     : "=a"(eax), "=d"(edx_dummy)
                     : "c"(0));
    return eax;
}

void pku_write_pkru(uint32_t value) {
    if (!cpu_has_pku()) return;
    __asm__ volatile(".byte 0x0F, 0x01, 0xEF"
                     :
                     : "a"(value), "c"(0), "d"(0)
                     : "memory");
}


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