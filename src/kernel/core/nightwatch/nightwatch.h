#ifndef NIGHTWATCH_H
#define NIGHTWATCH_H

#include "ktypes.h"


void nightwatch_init(void);

void nightwatch_core_idle(uint8_t core);

void nightwatch_core_busy(uint8_t core);

#endif