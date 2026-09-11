#ifndef HVCLOCK_H
#define HVCLOCK_H

#include "ktypes.h"


bool hvclock_init(void);
bool hvclock_is_available(void);

uint64_t hvclock_now_ns(void);

#endif