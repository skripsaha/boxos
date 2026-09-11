#ifndef BOX_CORE_RESULT_H
#define BOX_CORE_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "boxos_kctx.h"
#include "box/core/strand_self.h"

typedef struct PACKED {
    uint32_t error_code;
    uint32_t data_length;
    uint64_t data_addr;
    uint32_t sender_pid;
    uint32_t context;
} Result;

STATIC_ASSERT(sizeof(Result) == 24, "Result must be 24 bytes");

typedef struct PACKED {
    Result   r;
    uint64_t seq;
} ResultSlot;

STATIC_ASSERT(sizeof(ResultSlot) == 32, "ResultSlot must be 32 bytes");

typedef struct PACKED {
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    volatile uint64_t tail;
    volatile uint64_t awaiting;
    uint8_t           _pad_line1[48];
} ResultRingHeader;

STATIC_ASSERT(sizeof(ResultRingHeader) == 128,
              "ResultRingHeader must be 128 bytes (two cachelines)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, awaiting) == 72,
              "ResultRingHeader.awaiting must sit at offset 72 (kernel agrees)");

typedef struct PACKED {
    ResultRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(ResultRingHeader)];
} ResultRing;

STATIC_ASSERT(sizeof(ResultRing) == 4096, "ResultRing header must be one page");

INLINE ResultRing* result_ring(void) {
    return (ResultRing*)(uintptr_t)strand_rings().result_va;
}

INLINE bool result_ring_is_empty(const ResultRing* ring) {
    return ring->hdr.head == ring->hdr.tail;
}

bool result_available(void);
bool result_published_at_head(void);
uint32_t result_count(void);
bool result_pop(Result* out);
bool result_peek(Result* out);

bool result_wait(Result* out, uint32_t expect_cookie, uint32_t timeout_ms);

uint64_t result_orphans_dropped(void);

bool result_pop_non_ipc(Result* out);
bool result_pop_ipc(Result* out);
bool result_pop_any(Result* out);
void result_restash(const Result* r);
uint32_t result_ipc_stash_count(void);
uint32_t result_non_ipc_stash_count(void);

void result_stash_free_self(void);

void result_drain_orphan_replies(void);

bool result_pop_touch(Result* out);

bool result_wait_any(Result* out, uint32_t timeout_ms);

bool result_wait_ipc(Result* out, uint32_t timeout_ms);

bool result_pop_ferry(Result* out);
bool result_wait_ferry(Result* out, uint32_t timeout_ms);
uint32_t result_ferry_stash_count(void);

void result_pop_stats(uint64_t out[8]);


#ifdef __cplusplus
}
#endif

#endif