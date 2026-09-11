#ifndef PVCLOCK_H
#define PVCLOCK_H

#include "ktypes.h"


bool pvclock_init(void);
bool pvclock_is_available(void);

void pvclock_init_ap(uint8_t core_index);

uint64_t pvclock_now_ns(void);

bool pvclock_walltime(uint64_t *out_sec, uint32_t *out_nsec);

uint64_t pvclock_tsc_khz(void);

#endif