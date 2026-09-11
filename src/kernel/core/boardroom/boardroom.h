#ifndef BOARDROOM_H
#define BOARDROOM_H

#include "ktypes.h"
#include "error.h"


typedef enum {
    BOARD_NONE = 0,
    BOARD_AHCI,
    BOARD_ATA,
    BOARD_USB
} BoardKind;

#define BOARDROOM_SECTOR_BYTES 512

void BoardroomInit(void);

void BoardroomNoteArrival(void);

void BoardroomNoteDeparture(BoardKind kind, uint8_t index);

void BoardroomAttendIfPending(void);

typedef struct __attribute__((packed)) {
    uint8_t seated;
    uint8_t changed;
    uint8_t first;
    uint8_t reserved;
} BoardroomSeatEvent;

uint8_t     BoardroomSeatCount(void);
BoardKind   BoardroomSeatKind(uint8_t seat);
const char* BoardroomSeatName(uint8_t seat);

bool        BoardroomSeatIsRemovable(uint8_t seat);

bool        BoardroomSeatOccupied(uint8_t seat);

uint32_t    BoardroomSeatSeating(uint8_t seat);

uint32_t BoardroomSeatPhysicalBytes(uint8_t seat);

uint64_t BoardroomSeatSectors(uint8_t seat);

uint8_t BoardroomSeatIndex(uint8_t seat);

int BoardroomRead (uint8_t seat, uint64_t lba, uint32_t count, void* buffer);
int BoardroomWrite(uint8_t seat, uint64_t lba, uint32_t count, const void* buffer);

typedef void (*BoardroomAsyncCb)(uint8_t index, uint8_t slot,
                                 error_t status, void* ctx);

uint32_t BoardroomSeatRun(uint8_t seat);

error_t BoardroomReadAsync(uint8_t seat, uint64_t lba, uint32_t count,
                           void* dma_phys, BoardroomAsyncCb cb, void* ctx);

bool BoardroomSeatCanReadAsync(uint8_t seat);

void BoardroomProveUnattendedRead(uint8_t seat);

int BoardroomFlush(uint8_t seat);

typedef bool (*BoardroomProbe)(void* ctx, uint8_t seat, uint8_t out_uuid[16]);
uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx,
                            uint8_t out_uuid[16]);

#define BOARDROOM_NO_SEAT 0xFF

#endif