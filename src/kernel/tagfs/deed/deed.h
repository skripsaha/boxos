#ifndef DEED_READER_H
#define DEED_READER_H

#include "ktypes.h"
#include "error.h"
#include "volume_deed.h"
#include "ground.h"


STATIC_ASSERT(__builtin_offsetof(VolumeDeed, magic)          == 0,  "stage2 DEED_OFF_MAGIC");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, prologue_bytes) == 8,  "stage2 DEED_OFF_PROLOGUE_BYTES");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, stamp_bytes)    == 10, "stage2 DEED_OFF_STAMP_BYTES");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, crc32)          == 12, "stage2 DEED_OFF_CRC32");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, uuid)           == 16, "stage2 DEED_OFF_UUID");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, sectors)        == 32, "stage2 DEED_OFF_SECTORS");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, tail_sector)    == 40, "stage2 DEED_OFF_TAIL_SECTOR");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, role)           == 48, "stage2 DEED_OFF_ROLE");
STATIC_ASSERT(sizeof(VolumeDeed)                             == 52, "stage2 DEED_PROLOGUE_BYTES");

STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_block)  == 0, "stage2 BOOT_KERNEL_BLOCK");
STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_blocks) == 4, "stage2 BOOT_KERNEL_BLOCKS");
STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_bytes)  == 8, "stage2 BOOT_KERNEL_BYTES");

typedef struct {
    VolumeDeed     head;
    const uint8_t *stamps;
    uint8_t       *raw;
    uint32_t       raw_bytes;
} DeedCopy;

error_t DeedReadHead(uint8_t seat, const MediumGround *ground, DeedCopy *out);

error_t DeedReadTail(uint8_t seat, const MediumGround *ground,
                     const DeedCopy *head, DeedCopy *out);

error_t DeedReadTailAlone(uint8_t seat, const MediumGround *ground,
                          DeedCopy *out);

void DeedRelease(DeedCopy *copy);

const void *DeedStamp(const DeedCopy *copy, uint16_t kind, uint16_t *out_bytes);

void DeedDescribe(uint8_t seat, const DeedCopy *copy);

void DeedSurveyAll(uint8_t standing_seat, uint64_t standing_start);

#endif