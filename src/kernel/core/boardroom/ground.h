#ifndef GROUND_H
#define GROUND_H

#include "ktypes.h"


typedef enum {
    GROUND_FROM_MBR = 0,
    GROUND_FROM_GPT,
} GroundOrigin;

typedef struct {
    uint64_t     start_sector;
    uint64_t     sectors;
    GroundOrigin origin;
    uint8_t      entry;
} MediumGround;

#define GROUND_MAX_PER_MEDIUM 8

uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max);

const char *GroundOriginName(GroundOrigin origin);

#endif