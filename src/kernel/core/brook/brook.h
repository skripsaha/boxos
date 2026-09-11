#ifndef BROOK_H
#define BROOK_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "cabin_layout.h"
#include "boxos_magic.h"


#define BROOK_WRITER       0x01u
#define BROOK_READER       0x02u
#define BROOK_CREATE       0x10u
#define BROOK_STREAM       0x20u
#define BROOK_FLAGS_MASK   (BROOK_WRITER | BROOK_READER | BROOK_CREATE | BROOK_STREAM)

#define BROOK_ALIVE_FROZEN 0xFFFFFFFFu

#define BROOK_FRAME_SIZE_MIN     8u
#define BROOK_FRAME_SIZE_MAX     (64u * 1024u)

#define BROOK_FRAME_COUNT_MIN    2u
#define BROOK_FRAME_COUNT_MAX    16384u

#define BROOK_MAX_TOTAL_SIZE     (1ULL * 1024 * 1024 * 1024)

typedef struct BrookClaim BrookClaim;
typedef struct BrookObject BrookObject;
struct process_t;

#define BROOK_BELL_RUNG   0x80000000u
#define BROOK_BELL_PID(v) ((v) & ~BROOK_BELL_RUNG)

typedef struct {
    volatile uint64_t tail;
    volatile uint32_t writer_alive;
    volatile uint32_t writer_ever_attached;
    uint32_t          frame_size;
    uint32_t          frame_count;
    uint64_t          magic;
    volatile uint32_t writer_bell;
    uint8_t           _pad_line0[28];

    volatile uint64_t head;
    volatile uint32_t reader_alive;
    volatile uint32_t reader_ever_attached;
    volatile uint32_t reader_bell;
    uint8_t           _pad_line1[44];
} BrookHeader;

_Static_assert(sizeof(BrookHeader) == 128,
               "BrookHeader must be exactly two cachelines (128 B)");
_Static_assert(__builtin_offsetof(BrookHeader, tail) == 0,
               "BrookHeader.tail must be at offset 0 (CL0 = writer state)");
_Static_assert(__builtin_offsetof(BrookHeader, head) == 64,
               "BrookHeader.head must be at offset 64 (CL1 = reader state)");
_Static_assert(__builtin_offsetof(BrookHeader, writer_alive) < 64,
               "writer_alive must share CL0 with tail (UMWAIT correctness)");
_Static_assert(__builtin_offsetof(BrookHeader, reader_alive) >= 64,
               "reader_alive must share CL1 with head (UMWAIT correctness)");
_Static_assert(__builtin_offsetof(BrookHeader, reader_bell) >= 64,
               "reader_bell must share CL1 with head — the line the writer already reads");
_Static_assert(__builtin_offsetof(BrookHeader, writer_bell) < 64,
               "writer_bell must share CL0 with tail — the line the reader already reads");

struct BrookObject {
    BrookObject  *bucket_next;

    uint16_t      tag_id;
    uint16_t      slot_page_class;
    uint32_t      flags;
    uint32_t      frame_size;
    uint32_t      frame_count;

    uint64_t      header_phys;
    uint64_t      slot_total_size;
    uint64_t      slot_chunk_size;
    uint32_t      slot_chunk_count;
    uint32_t      _pad0;
    uint64_t     *slot_chunks;

    uint32_t      writer_pid;
    uint32_t      reader_pid;
    uint32_t      ref_count;
    uint32_t      _pad1;

    uint64_t      create_pid;
    uint64_t      create_tsc;
};

struct BrookClaim {
    BrookObject  *brook;
    BrookClaim   *proc_next;
    uint64_t      user_va_header;
    uint64_t      user_va_slots;
    uint32_t      role;
    uint32_t      flags;
    struct process_t *proc;
};

void BrookInit(void);

error_t BrookOpenInternal(struct process_t *proc,
                          const char *tag,
                          uint32_t frame_size,
                          uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_user_va_header,
                          uint64_t *out_user_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count);

error_t BrookReleaseInternal(struct process_t *proc,
                             uint64_t user_va_header);

void BrookCleanupProcess(struct process_t *proc);

void BrookStatsSnapshot(uint64_t out[4]);

bool BrookBellUnrung(uint32_t reader_pid, uint16_t *out_tag_id,
                     uint64_t *out_head, uint64_t *out_tail);

#endif