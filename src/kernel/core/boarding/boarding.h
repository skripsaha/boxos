#ifndef BOARDING_H
#define BOARDING_H

#include "ktypes.h"
#include "boarding_pass.h"


void BoardingPassInit(void);

bool BoardingPassPresent(void);

const void* BoardingPassStamp(uint16_t kind, uint16_t* out_bytes);

bool BoardingPassVolume(uint8_t out_uuid[16]);

bool BoardingPassMedium(uint8_t* out_firmware, uint8_t* out_bios_drive);

#endif