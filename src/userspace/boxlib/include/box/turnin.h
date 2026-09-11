#ifndef BOX_TURNIN_H
#define BOX_TURNIN_H

#include "box/types.h"


typedef struct {
    uint64_t touch;
    uint64_t result;
} TurnInMark;

TurnInMark box_mark(void);

bool box_mark_moved(TurnInMark seen);

void box_turn_in(TurnInMark seen);

#endif