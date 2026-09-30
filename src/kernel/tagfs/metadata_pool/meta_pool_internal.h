#ifndef META_POOL_INTERNAL_H
#define META_POOL_INTERNAL_H

#include "meta_pool.h"

/*
 * The pool's own parts, shared by meta_pool.c (the chain, the mount, the
 * mirror, where a record sits) and meta_pool_tail.c (the tail, the closed
 * blocks, the Spares, what a cycle writes). Everything here is read and
 * changed under the volume's state lock, except a page's image on its way to
 * the medium, which only the Settle's holder reads, and the mount's walk,
 * which runs with the door shut.
 */

/* The 16-byte block header, then records packed end to end, each opening with
 * its u16 length and u32 file id. */
#define MPOOL_BLOCK_HEADER  16

/* One link of the chain: the block, the number it keeps whatever is unlinked
 * around it — the numbers only rise along the chain — and, once the block is
 * closed, its mix in the seal's fold (PoolBlockMix); 0 for the open tail. */
typedef struct {
    uint32_t block;
    uint32_t link;
    uint32_t mix;
} MetaPoolLink;

extern MetaPoolLink* g_pool_chain;
extern uint32_t      g_pool_chain_len;
extern uint32_t      g_pool_chain_cap;

/* The seal's closed_crc32 as memory has it: every closed link's mix, XORed
 * (volume_ledger.h). A link closed XORs its mix in; one dropped off the front
 * XORs it out, and nothing is read. */
extern uint32_t      g_pool_closed_fold;

/* A block's mix in the fold: the CRC32 of its records, summed again with the
 * block's number and their length. The block number names the link, and it is
 * the one name a link keeps across mounts and across blocks unlinked before it
 * (a link's number is memory's, counted afresh by every mount). */
uint32_t PoolBlockMix(uint32_t block, const MetaPoolBlock* blk);

/*
 * A block of the pool that is in memory and not yet the medium's: a closed
 * block no settled cycle has written, or a Spare. One shape, because a Spare's
 * page is what the tail it closes becomes: the Spare's block and image turn
 * into the new tail, and the page carries the closed block away.
 */
struct MetaPoolPage {
    MetaPoolPage*  next;
    uint32_t       block;
    uint32_t       link;
    MetaPoolBlock* image;
    uint64_t       gen_max;     /* a closed block: the newest record it holds */
    uint64_t       need;        /* a Spare: its stamp's; a closed block: the stamp its link names */
};

/* The tail: the chain's last link, in memory. `frozen` is how much of its
 * payload a cycle has taken — settled or in flight — and is never written
 * over again by an Ink; `changed` says the medium has not taken what memory
 * holds. */
extern MetaPoolBlock* g_pool_tail;
extern uint32_t       g_pool_tail_block;
extern uint32_t       g_pool_tail_link;
extern uint32_t       g_pool_tail_frozen;
extern uint64_t       g_pool_tail_gen;
extern bool           g_pool_tail_changed;

/* Room for `want` links, made with no state lock held: the array is swapped in
 * under it. 0, or -1 when the heap refused. */
int      PoolChainRoom(uint32_t want);

/* A link appended; the room was made. */
void     PoolChainAppend(uint32_t block);

/* Where in the chain link `link` sits, or g_pool_chain_len. */
uint32_t PoolChainAt(uint32_t link);

/* How many records of the block a file still holds: those with a file id. */
uint16_t PoolCountKept(const MetaPoolBlock* blk);

/* The image of link `link` when memory holds the block's newest bytes — the
 * tail, or a closed block no settled cycle has written — else NULL. */
const MetaPoolBlock* PoolImageOf(uint32_t link);

/* The tail, the closed blocks and the Spares, let go with the volume. */
void     PoolTailShutdown(void);

#endif /* META_POOL_INTERNAL_H */
