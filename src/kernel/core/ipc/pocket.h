#ifndef POCKET_H
#define POCKET_H

#include "ktypes.h"
#include "klib.h"
#include "boxos_decks.h"
#include "error.h"


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

#endif