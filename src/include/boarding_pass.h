#ifndef BOARDING_PASS_H
#define BOARDING_PASS_H

#include "ktypes.h"


#define BOARDING_PASS_ADDR    0xA600
#define BOARDING_PASS_BYTES   512

#ifdef BOARDING_PASS_ADDR_FROM_BUILD
_Static_assert(BOARDING_PASS_ADDR == BOARDING_PASS_ADDR_FROM_BUILD,
               "the boarding pass address disagrees with the image build: the "
               "loaders write the block where the Makefile says and the kernel "
               "reads it where this header says, and a boot only finds out "
               "when the pass comes back blank");
#endif
#define BOARDING_PASS_MAGIC   0x53415042u
#define BOARDING_PASS_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint16_t used_bytes;
    uint16_t capacity;
    uint16_t count;
    uint16_t reserved;
} BoardingPassHeader;

_Static_assert(sizeof(BoardingPassHeader) == 16, "a boarding pass header is 16 bytes");

typedef struct __attribute__((packed)) {
    uint16_t kind;
    uint16_t bytes;
} BoardingStampHeader;

_Static_assert(sizeof(BoardingStampHeader) == 4, "a stamp header is 4 bytes");

#define BOARDING_STAMP_VOLUME  1
#define BOARDING_STAMP_MEDIUM  2
#define BOARDING_STAMP_LOADER  3
#define BOARDING_STAMP_SEAL    4

typedef struct __attribute__((packed)) {
    uint8_t uuid[16];
} BoardingVolume;

#define BOARDING_FIRMWARE_BIOS 0
#define BOARDING_FIRMWARE_UEFI 1

typedef struct __attribute__((packed)) {
    uint8_t  firmware;
    uint8_t  bios_drive;
    uint16_t reserved;
} BoardingMedium;

_Static_assert(sizeof(BoardingMedium) == 4, "a medium stamp is 4 bytes");

typedef struct __attribute__((packed)) {
    char     name[12];
    uint16_t major;
    uint16_t minor;
} BoardingLoader;

_Static_assert(sizeof(BoardingLoader) == 16, "a loader stamp is 16 bytes");

typedef struct __attribute__((packed)) {
    uint32_t crc32;
} BoardingSeal;

_Static_assert(sizeof(BoardingSeal) == 4, "a seal stamp is 4 bytes");

#endif