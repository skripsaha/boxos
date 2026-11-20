#ifndef BOX_CORE_HEAP_BLOCKS_H
#define BOX_CORE_HEAP_BLOCKS_H

/*
 * The global heap's blocks — boxlib's own, for memory.c and nobody else.
 *
 * Every byte from the heap's base to its end belongs to one block: a 32-byte
 * header and its payload, laid end to end, each header knowing the block just
 * below it. A free block is also on one of the free lists, kept by size, so a
 * block that fits is found, and a block given back is merged with free
 * neighbours, in a time that does not depend on how many blocks the heap
 * holds (heap_blocks.c).
 *
 * Every function here is called with heap_lock held (memory.c), save
 * block_of / payload_of, which only reckon.
 */

#include "box/defs.h"
#include "box/error.h"

#define HEAP_MAGIC      0xA110CA7EU
#define HEAP_ALIGN      16

/* What a block is. A FOREIGN block is a range a large allocation was given
 * where a mapping not the heap's lies: it is never given out, never merged
 * and never touched, and only its header — below the range — is written. */
enum {
    BLOCK_USED    = 0,
    BLOCK_FREE    = 1,
    BLOCK_FOREIGN = 2,
};

typedef struct block {
    size_t        size;    // payload bytes, past the header
    uint32_t      magic;
    uint32_t      state;   // BLOCK_USED, BLOCK_FREE or BLOCK_FOREIGN
    struct block* prev;    // the block just below in memory; NULL for the first
    uint8_t       tag;     // HEAP_TAG_NONE (0xFF) = untagged
    uint8_t       _pad[7]; // keep the header at 32 bytes
} block_t;

#define BLOCK_HDR_SIZE  32u

_Static_assert(sizeof(block_t) == BLOCK_HDR_SIZE, "a block's header is 32 bytes");

static inline block_t *block_of(void *payload) {
    return (block_t *)((uint8_t *)payload - BLOCK_HDR_SIZE);
}

static inline void *payload_of(block_t *b) {
    return (uint8_t *)b + BLOCK_HDR_SIZE;
}

/* A block of at least `size` bytes, USED and tagged `tag`: a free one that
 * fits, or one the heap grows by. NULL with the cause in *errcell when there
 * is none. A block of 2 MiB or more that the heap grew by comes back with the
 * range under it in *huge_va / *huge_len — 2 MiB-aligned, not a byte of it
 * touched, its header below it — for the caller to ask the kernel to back
 * with large pages once the lock is dropped; otherwise *huge_len is 0. */
void   *heap_blocks_take(size_t size, uint8_t tag, error_t *errcell,
                         uintptr_t *huge_va, size_t *huge_len);

/* A USED block given back: FREE, merged with a free neighbour below and
 * above, and on its list. ERR_CORRUPTED when a neighbour's header does not
 * agree with it — the block is given back all the same, not merged with
 * that neighbour — else OK. */
error_t heap_blocks_give(block_t *b);

/* A USED block made `size` bytes where it stands — shrunk, the rest given
 * back, or grown into the free block above it or into the heap's end. False
 * when it cannot grow where it stands. */
bool    heap_blocks_resize(block_t *b, size_t size);

/* A USED block cut to the first `size` bytes past a payload aligned to
 * `alignment` (a power of two above HEAP_ALIGN): what lies in front of that
 * is given back, and so is what lies past it. The block must hold at least
 * size + alignment + one header + HEAP_ALIGN. The payload that is left. */
void   *heap_blocks_align(block_t *b, size_t alignment, size_t size);

/* A block the heap grew by for a large allocation, whose range meets a
 * mapping not the heap's: FOREIGN from now on. */
void    heap_blocks_bar(block_t *b);

/* The blocks in memory order: the first, then each after the one before,
 * NULL past the last. A header that is not one is given as it lies, for the
 * caller to stop at: its magic is not HEAP_MAGIC. */
block_t *heap_blocks_first(void);
block_t *heap_blocks_next(block_t *b);

/* The bytes the heap has grown by. */
size_t  heap_blocks_grown(void);

#endif /* BOX_CORE_HEAP_BLOCKS_H */
