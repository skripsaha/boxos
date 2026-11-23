#include "block_map_derived.h"
#include "../tagfs.h"
#include "tagfs_volume.h"

/*
 * A look at the derived map from outside its changes (block_map_derived.h,
 * BlockMapDerivedTakenFrom): the first block of a range an owner names, a few
 * windows a hold of the state lock — the lock every change of the live map is
 * made under — so a long range never keeps the lock long. What a Tally is
 * reckoned to cost asks it, of the ranges of DIGESTS the keys that changed
 * meet (settle/binder_live.c). Its own file beside the map's, which keeps its
 * pages and its changes.
 */

#define LOOK_WINDOWS  64u   /* windows a hold of the state lock looks at */

/* The first bit of `bits` set in [from, to), or `to`. */
static uint32_t BitsFirstIn(const uint64_t* bits, uint32_t from, uint32_t to)
{
    for (uint32_t at = from, n; at < to; at += n) {
        uint32_t off  = at % 64u;
        n             = to - at < 64u - off ? to - at : 64u - off;
        uint64_t word = bits[at / 64u] & (n == 64u ? ~0ull : ((1ull << n) - 1u) << off);
        if (word)
            return at - off + (uint32_t)__builtin_ctzll(word);
    }
    return to;
}

uint64_t BlockMapDerivedTakenFrom(TagFSVolume* vol, uint64_t start, uint64_t end)
{
    TagFSLock* lock  = &vol->state.lock;
    uint64_t   at    = start, first = end;
    bool       known = true;
    while (known && first == end && at < end) {
        TagFSLockTake(lock);
        for (uint32_t n = 0; first == end && at < end && n < LOOK_WINDOWS; n++) {
            uint64_t        at_w = at / BLOCK_MAP_WINDOW_BLOCKS;
            uint32_t        w    = at_w < UINT32_MAX ? (uint32_t)at_w : UINT32_MAX, blocks = 0, free = 0;
            const uint64_t* bits = NULL;
            uint64_t        base = at_w * BLOCK_MAP_WINDOW_BLOCKS;
            if (!BlockMapDerivedWindow(vol, w, &blocks, &free, &bits) || base + blocks <= at) {
                known = BlockMapDerivedUp(vol);       /* past the map's end no owner names a block */
                at    = end;
                break;
            }
            uint64_t past = base + blocks < end ? base + blocks : end;
            if (bits)
                first = base + BitsFirstIn(bits, (uint32_t)(at - base), (uint32_t)(past - base));
            else if (free == 0)
                first = at;
            first = first < past ? first : end;
            at    = past;
        }
        TagFSLockDrop(lock);
    }
    return known ? first : start;
}
