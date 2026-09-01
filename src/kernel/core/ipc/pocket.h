#ifndef POCKET_H
#define POCKET_H

#include "ktypes.h"
#include "klib.h"
#include "boxos_decks.h"
#include "error.h"

/*
 * Pocket — kernel-bound syscall envelope.
 *
 * The struct, flags and enclosure geometry live in the shared ABI header
 * boxos_pocket.h (single source of truth for kernel and boxlib — the same
 * arrangement Crate and Manifest already have). This file adds the
 * kernel-side accessors only.
 *
 * The 128-byte slot stride halves PocketRing capacity against the 64-byte
 * era (POCKET_RING_SLOT_MAX 16384 → 8192 within the same 1 MiB slot
 * reservation) and buys the enclosure: a Manifest that fits rides inside
 * the envelope, so the kernel never reads it from cabin memory whose
 * lifetime it cannot see. Capacity is a throughput knob, never a
 * correctness boundary — a full ring back-pressures the push.
 */

#include "boxos_pocket.h"

static inline uint64_t PocketManifestAddr(const Pocket *p)
{
    return p ? p->manifest_addr : 0;
}

static inline uint32_t PocketManifestSize(const Pocket *p)
{
    return p ? p->manifest_size : 0;
}

static inline uint64_t PocketCratesAddr(const Pocket *p)
{
    return p ? p->crates_addr : 0;
}

static inline uint16_t PocketCrateCount(const Pocket *p)
{
    return p ? p->crate_count : 0;
}

static inline uint16_t PocketPierId(const Pocket *p)
{
    return p ? p->pier_id : 0;
}

static inline void pocket_init(Pocket *p)
{
    if (!p) return;
    memset(p, 0, sizeof(Pocket));
}

#endif /* POCKET_H */
