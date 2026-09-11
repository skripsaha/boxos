#ifndef CPU_CAPS_PAGE_H
#define CPU_CAPS_PAGE_H

#include "ktypes.h"
#include "boxos_magic.h"
#include "boxos_addresses.h"

#define CPU_CAPS_PAGE_MAGIC 0x43505543

typedef struct __packed {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint16_t _pad0;
    uint64_t tsc_freq_khz;
    bool has_pku;
    bool has_pks;
    bool has_lam;
    bool has_cet;
    bool has_tme;
    bool has_fsgsbase;
    bool has_rdrand;
    bool has_rdseed;
    uint8_t _reserved[4072];
} cpu_caps_page_t;

STATIC_ASSERT(sizeof(cpu_caps_page_t) == 4096, "CPU caps page must be exactly 4096 bytes");

extern uint64_t g_cpu_caps_page_phys;

void cpu_caps_page_init(void);
void cpu_caps_page_set_tsc_freq(uint64_t freq_khz);

void cpu_caps_page_refresh_features(void);

#endif