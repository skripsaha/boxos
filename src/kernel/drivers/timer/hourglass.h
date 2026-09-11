#ifndef HOURGLASS_H
#define HOURGLASS_H

#include "ktypes.h"


typedef enum {
    HOURGLASS_RULER_NONE = 0,
    HOURGLASS_RULER_TSC,
    HOURGLASS_RULER_PMTIMER,
    HOURGLASS_RULER_PIT8254,
} HourGlassRuler;

typedef struct {
    HourGlassRuler ruler;
    uint64_t span;
    uint64_t elapsed;
    uint64_t hz;
    uint64_t mark;
    uint64_t witness;
    uint64_t stall;
    uint32_t last;
    uint32_t mask;
    bool     died;
} HourGlass;

bool HourGlassTurn(HourGlass *g, uint64_t us);

bool HourGlassTurnOn(HourGlass *g, uint64_t us, HourGlassRuler ruler);

bool HourGlassRunOut(HourGlass *g);

uint64_t HourGlassElapsedUs(const HourGlass *g);

bool HourGlassSourceDied(const HourGlass *g);

const char *HourGlassRulerName(const HourGlass *g);

#endif