#ifndef POCKET_H
#define POCKET_H

#include "ktypes.h"
#include "klib.h"
#include "boxos_sizes.h"
#include "boxos_decks.h"
#include "error.h"

// Pocket: a syscall request from userspace to kernel.
// Lives in PocketRing (per-process shared memory).
// Data is NOT inline — data_addr points to cabin heap.

typedef struct __packed {
    uint32_t pid;                // kernel overwrites from process_t (security)
    uint32_t target_pid;         // 0 = result to self, != 0 = IPC route
    uint32_t error_code;         // deck handlers write errors here
    uint8_t  prefix_count;       // number of valid prefixes
    uint8_t  current_prefix_idx; // current position in chain (for async resume)
    uint8_t  flags;              // POCKET_FLAG_YIELD = 0x80
    uint8_t  _reserved;
    uint32_t data_length;        // bytes of valid data at data_addr
    uint64_t data_addr;          // virtual address of data in process cabin heap
    char     route_tag[POCKET_ROUTE_TAG_SIZE];  // for ROUTE_TAG broadcasts
    uint16_t prefixes[POCKET_MAX_PREFIXES];     // deck_id << 8 | opcode, last = 0x0000
    uint8_t  _pad[4];            // padding to reach 96 bytes for PocketRing packing
} Pocket;

_Static_assert(sizeof(Pocket) == 96, "Pocket must be 96 bytes for PocketRing packing");

#define POCKET_FLAG_YIELD 0x80

// Pocket helpers

static inline void pocket_init(Pocket* p)
{
    if (!p) return;
    memset(p, 0, sizeof(Pocket));
}

static inline uint8_t pocket_get_deck_id(const Pocket* p, uint8_t idx)
{
    if (!p || idx >= POCKET_MAX_PREFIXES) return 0xFF;
    return DECK_ID_FROM_PREFIX(p->prefixes[idx]);
}

static inline uint8_t pocket_get_opcode(const Pocket* p, uint8_t idx)
{
    if (!p || idx >= POCKET_MAX_PREFIXES) return 0xFF;
    return OPCODE_FROM_PREFIX(p->prefixes[idx]);
}

static inline uint16_t pocket_current_prefix(const Pocket* p)
{
    if (!p || p->current_prefix_idx >= POCKET_MAX_PREFIXES) return PREFIX_TERMINATOR;
    return p->prefixes[p->current_prefix_idx];
}

static inline void pocket_advance(Pocket* p)
{
    if (p && p->current_prefix_idx < POCKET_MAX_PREFIXES) {
        p->current_prefix_idx++;
    }
}

static inline bool pocket_validate(const Pocket* p)
{
    return p &&
           p->current_prefix_idx < POCKET_MAX_PREFIXES &&
           p->prefix_count <= POCKET_MAX_PREFIXES;
}

#endif // POCKET_H
