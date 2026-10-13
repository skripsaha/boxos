#ifndef TAGFS_TRACT_H
#define TAGFS_TRACT_H

#include "file_record.h"

/*
 * The Tract and its Parcels (file_record.h) as tract.c builds them: for
 * file_record.c, and for the stand that counts what a change copied.
 *
 * A Parcel holds its Strips and, after them, where each ends among the
 * Parcel's blocks (ParcelEnds), so a block is found inside it by halving. A
 * Tract node names its kids — Parcels at level 0, nodes one level down above
 * it — each with the blocks under it and every kid before it, so a block is
 * found down the tree by halving too. Every leaf of a Tract is at the same
 * depth, and every node but the root names at least TRACT_KIDS_LEAST kids.
 *
 * Both are immutable once built and shared by reference: a node takes a
 * reference on every kid it names and lets them go when its own last one
 * goes. `bytes` is what the heap holds for the node and everything under it
 * — counted as if nothing were shared, which no two files ever do.
 */

struct Parcel {
    volatile uint32_t refs;
    uint32_t          count;     /* Strips, 1 .. PARCEL_STRIPS_MOST */
    uint64_t          blocks;
    Strip             strip[];   /* `count` of them, then ParcelEnds */
};

typedef struct {
    uint64_t    end;    /* the blocks under this kid and every kid before it */
    const void* node;   /* a Parcel at level 0, else a Tract one level down */
} TractKid;

struct Tract {
    volatile uint32_t refs;
    uint16_t          level;     /* 0: the kids are Parcels */
    uint16_t          count;     /* kids, 1 .. TRACT_KIDS_MOST */
    uint64_t          strips;
    uint64_t          bytes;
    TractKid          kid[];
};

/* Where each Strip of the Parcel ends among its blocks. */
static inline const uint64_t* ParcelEnds(const Parcel* p)
{
    return (const uint64_t*)(const void*)&p->strip[p->count];
}

static inline uint64_t ParcelBytes(uint32_t count)
{
    return sizeof(Parcel) + (uint64_t)count * (sizeof(Strip) + sizeof(uint64_t));
}

static inline uint64_t TractBlocks(const Tract* t)
{
    return t ? t->kid[t->count - 1u].end : 0;
}

/* The Tract of `count` Strips cut into Parcels, with one reference: OK with
 * *out NULL for none; ERR_INVALID_ARGUMENT for a Strip of no block or of a
 * kind memory does not know, ERR_NO_MEMORY — and nothing built. */
error_t TractOfStrips(const Strip* strips, uint64_t count, const Tract** out);

/* A list of ground that grows as it is added to (TractGroundAdd: OK or
 * ERR_NO_MEMORY); its Strips are the caller's to free. */
typedef struct {
    Strip*   strip;
    uint64_t count;
    uint64_t room;
} TractGround;

error_t TractGroundAdd(TractGround* g, const Strip* s);

/* One splice applied (file_record.h, BookSplice) to the Tract `root` — NULL
 * for none: the new Tract with one reference in *out (NULL for one that names
 * nothing), `root` untouched. The DATA ground it takes out is added to `took`
 * when that is not NULL. OK; ERR_INVALID_ARGUMENT for a splice past the runs'
 * end or inserting a Strip of no block or of a kind memory does not know;
 * ERR_NO_MEMORY — and nothing built. */
error_t TractSplice(const Tract* root, const BookSplice* sp, const Tract** out, TractGround* took);

/* Block k under `root`: the Parcel holding it and k counted inside it; false
 * past the runs' end. *steps grows by every halving step taken. */
bool    TractFind(const Tract* root, uint64_t k, const Parcel** out, uint64_t* k_in,
                  uint64_t* steps);

void    TractHold(const Tract* t);
void    TractLetGo(const Tract* t);

#endif /* TAGFS_TRACT_H */
