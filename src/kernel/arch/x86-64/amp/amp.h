#ifndef AMP_H
#define AMP_H

#include "ktypes.h"

#define MAX_CORES           256
#define AP_TRAMPOLINE_PHYS  0x8000

typedef struct {
    uint32_t lapic_id;
    uint8_t  core_index;
    bool     is_bsp;
    bool     is_kcore;
    uint8_t  online;
    uint8_t  counted_at_boot;
} CoreDescriptor;

typedef struct {
    CoreDescriptor cores[MAX_CORES];
    uint8_t  total_cores;
    uint8_t  k_count;
    uint8_t  app_count;
    uint8_t  bsp_index;
    uint32_t bsp_lapic_id;
    bool     multicore_active;
} AmpLayout;

extern AmpLayout g_amp;

void amp_init(void);
void amp_boot_aps(void);
uint8_t amp_get_core_index(void);
bool amp_is_kcore(void);
bool amp_is_appcore(void);
uint32_t amp_calculate_kcores(uint32_t total_cores);

static inline bool amp_core_online(const CoreDescriptor *c) {
    return __atomic_load_n(&c->online, __ATOMIC_ACQUIRE) != 0;
}

#endif