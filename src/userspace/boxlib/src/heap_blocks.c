/*
 * heap_blocks.c — the global heap's blocks. See box/core/heap_blocks.h.
 *
 * A free block is on one of the free lists by its size, in two levels: the
 * power of two its size lies under, then one of sixteen equal steps within
 * that power — below 256 bytes one list for every 16. Each level's lists that
 * hold a block are marked in a bitmap, so the first list sure to fit a request
 * is found by two bit scans, and a block's own list by its size. Two free
 * blocks never stand side by side: a block given back is merged at once with
 * a free one below it (its header's prev) and above it (past its payload).
 * Nothing walks the heap but the diagnostics.
 *
 * A free block keeps its list's links in its payload, which is why a payload
 * is never smaller than 16 bytes. A header is trusted only as far as it
 * agrees with its neighbours — its magic, the block below pointing past
 * itself to it, the block above naming it as below — and a block or a link
 * that does not agree is left where it is, never followed into.
 */

#include "box/core/heap_blocks.h"
#include "box/core/cabin.h"
#include "box/memory.h"
#include "boxos_sizes.h"
#include "cabin_layout.h"

#define HEAP_MIN_SPLIT     (BLOCK_HDR_SIZE + HEAP_ALIGN)   /* the least a block can be: a header and 16 bytes */
#define HEAP_HUGE          LARGE_PAGE_2M_SIZE

#define SL_LOG2            4u
#define SL_COUNT           (1u << SL_LOG2)
#define SMALL_SHIFT        8u                              /* below 1 << 8 = 256 bytes, one list per 16 */
#define LARGEST_LOG2       36u                             /* every block is smaller than 64 GiB */
#define FL_COUNT           (LARGEST_LOG2 - SMALL_SHIFT + 1u)

_Static_assert(HEAP_ALIGN << SL_LOG2 == 1u << SMALL_SHIFT, "the small lists are one per HEAP_ALIGN");
_Static_assert(FL_COUNT <= 64, "the first level's lists are marked in one 64-bit word");
_Static_assert((uint64_t)CABIN_HEAP_MAX_SIZE < (1ULL << LARGEST_LOG2), "a heap is smaller than its largest block can be");

typedef struct {
    block_t *next, *prev;
} free_links_t;

static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;   /* the heap's end: the last block ends here */
static uintptr_t heap_max     = 0;
static block_t  *heap_last    = NULL;
static int       initialized  = 0;

static block_t  *free_heads[FL_COUNT][SL_COUNT];
static uint16_t  sl_bitmap[FL_COUNT];
static uint64_t  fl_bitmap;

static void heap_init(void) {
    CabinInfo *ci = cabin_info();
    if (ci->heap_base != 0 && ci->heap_max_size != 0) {
        heap_base = ci->heap_base;
        heap_max  = heap_base + ci->heap_max_size;
    } else {
        heap_base = CABIN_HEAP_BASE;
        heap_max  = heap_base + CABIN_HEAP_MAX_SIZE;
    }
    if (heap_max - heap_base >= (1ULL << LARGEST_LOG2))
        heap_max = heap_base + (1ULL << LARGEST_LOG2) - HEAP_HUGE;
    heap_current = heap_base;
    initialized  = 1;
}

static inline size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline unsigned high_bit(size_t v) {
    return 63u - (unsigned)__builtin_clzll((unsigned long long)v);
}

static inline free_links_t *links_of(block_t *b) {
    return (free_links_t *)payload_of(b);
}

static inline block_t *block_after(block_t *b) {
    return (block_t *)((uint8_t *)payload_of(b) + b->size);
}

/* Whether x is where a header of this heap could be, and is one. */
static bool header_at(const block_t *x) {
    const uintptr_t at = (uintptr_t)x;
    return at >= heap_base && at <= heap_current - BLOCK_HDR_SIZE &&
           (at & (HEAP_ALIGN - 1)) == 0 && x->magic == HEAP_MAGIC;
}

/* ---- the free lists ------------------------------------------------------- */

/* The list a free block of `size` bytes is put on. */
static void list_for(size_t size, unsigned *fl, unsigned *sl) {
    if (size < (1u << SMALL_SHIFT)) {
        *fl = 0;
        *sl = (unsigned)(size / HEAP_ALIGN);
        return;
    }
    const unsigned top = high_bit(size);
    *fl = top - (SMALL_SHIFT - 1u);
    *sl = (unsigned)(size >> (top - SL_LOG2)) ^ SL_COUNT;
}

/* The first list every block of which holds `size` bytes: the size raised to
 * the next step's start. */
static void list_fitting(size_t size, unsigned *fl, unsigned *sl) {
    if (size >= (1u << SMALL_SHIFT))
        size += ((size_t)1 << (high_bit(size) - SL_LOG2)) - 1;
    list_for(size, fl, sl);
}

static void list_insert(block_t *b) {
    unsigned fl, sl;
    list_for(b->size, &fl, &sl);
    free_links_t *l    = links_of(b);
    block_t      *head = free_heads[fl][sl];
    l->next = head;
    l->prev = NULL;
    if (head) links_of(head)->prev = b;
    free_heads[fl][sl] = b;
    sl_bitmap[fl] |= (uint16_t)(1u << sl);
    fl_bitmap     |= 1ULL << fl;
}

/* A list emptied of what it held: what it held stays as it lies, on no list. */
static void list_drop(unsigned fl, unsigned sl) {
    free_heads[fl][sl] = NULL;
    sl_bitmap[fl] &= (uint16_t)~(1u << sl);
    if (sl_bitmap[fl] == 0) fl_bitmap &= ~(1ULL << fl);
}

/* b taken off its list — false, and nothing changed, when its links do not
 * agree with its neighbours on the list. */
static bool list_remove(block_t *b) {
    unsigned fl, sl;
    list_for(b->size, &fl, &sl);
    free_links_t *l    = links_of(b);
    block_t      *next = l->next, *prev = l->prev;
    if (next && (!header_at(next) || links_of(next)->prev != b)) return false;
    if (prev ? (!header_at(prev) || links_of(prev)->next != b) : free_heads[fl][sl] != b) return false;
    if (next) links_of(next)->prev = prev;
    if (prev) {
        links_of(prev)->next = next;
    } else {
        free_heads[fl][sl] = next;
        if (!next) list_drop(fl, sl);
    }
    return true;
}

/* A free block of at least `size` bytes, and the list it heads: the head of
 * the list a block of `size` would go on, when it is large enough, or else the
 * head of the first list every block of which is. NULL when no list has one. */
static block_t *find_free(size_t size, unsigned *fl, unsigned *sl) {
    list_for(size, fl, sl);
    block_t *b = free_heads[*fl][*sl];
    if (b && b->magic == HEAP_MAGIC && b->size >= size) return b;

    list_fitting(size, fl, sl);
    if (*fl >= FL_COUNT) return NULL;
    uint32_t sl_map = sl_bitmap[*fl] & (~0u << *sl);
    if (!sl_map) {
        const uint64_t fl_map = *fl + 1 < FL_COUNT ? fl_bitmap & (~0ULL << (*fl + 1)) : 0;
        if (!fl_map) return NULL;
        *fl    = (unsigned)__builtin_ctzll(fl_map);
        sl_map = sl_bitmap[*fl];
    }
    *sl = (unsigned)__builtin_ctz(sl_map);
    return free_heads[*fl][*sl];
}

/* ---- neighbours ----------------------------------------------------------- */

/* The block just above b, agreeing b is below it; NULL when b is the last,
 * and NULL with ERR_CORRUPTED in *why when what lies above does not agree. */
static block_t *above_of(block_t *b, error_t *why) {
    if (b == heap_last) return NULL;
    block_t *n = block_after(b);
    if (!header_at(n) || n->prev != b) {
        *why = ERR_CORRUPTED;
        return NULL;
    }
    return n;
}

/* The block just below b, agreeing b lies past it; NULL when b is the first,
 * and NULL with ERR_CORRUPTED in *why when it does not agree. */
static block_t *below_of(block_t *b, error_t *why) {
    block_t *p = b->prev;
    if (!p) return NULL;
    if (!header_at(p) || block_after(p) != b) {
        *why = ERR_CORRUPTED;
        return NULL;
    }
    return p;
}

/* hi, just above lo, made part of it; beyond is the block above hi, NULL
 * when hi is the last. Neither is on a list. */
static void join(block_t *lo, block_t *hi, block_t *beyond) {
    lo->size += BLOCK_HDR_SIZE + hi->size;
    hi->magic = 0;
    if (beyond) beyond->prev = lo;
    else        heap_last = lo;
}

error_t heap_blocks_give(block_t *b) {
    if (b->state != BLOCK_USED) return ERR_INVALID_ADDRESS;
    error_t  why   = OK;
    block_t *above = above_of(b, &why);
    block_t *below = below_of(b, &why);
    const bool known_above = above || b == heap_last;
    b->state = BLOCK_FREE;
    b->tag   = HEAP_TAG_NONE;

    if (above && above->state == BLOCK_FREE) {
        error_t  seen   = OK;
        block_t *beyond = above_of(above, &seen);
        if (seen == OK && list_remove(above)) {
            join(b, above, beyond);
            above = beyond;
        } else {
            why = ERR_CORRUPTED;
        }
    }
    if (below && below->state == BLOCK_FREE && known_above) {
        if (list_remove(below)) {
            join(below, b, above);
            b = below;
        } else {
            why = ERR_CORRUPTED;
        }
    }
    list_insert(b);
    return why;
}

/* b, USED, cut to `size` bytes, what lies past them given back when it can
 * be a block of its own; b left whole when the block above it does not agree. */
static void cut(block_t *b, size_t size) {
    if (b->size < size + HEAP_MIN_SPLIT) return;
    error_t  seen   = OK;
    block_t *beyond = above_of(b, &seen);
    if (seen != OK) return;
    block_t *rest = (block_t *)((uint8_t *)payload_of(b) + size);
    rest->size  = b->size - size - BLOCK_HDR_SIZE;
    rest->magic = HEAP_MAGIC;
    rest->state = BLOCK_USED;
    rest->prev  = b;
    rest->tag   = HEAP_TAG_NONE;
    b->size = size;
    if (beyond) beyond->prev = rest;
    else        heap_last = rest;
    (void)heap_blocks_give(rest);
}

/* ---- growth --------------------------------------------------------------- */

/* A block of `size` bytes from the heap's end: the free block that ends it
 * grown by what it lacks, or a block laid past it. */
static void *grow(size_t size, uint8_t tag, error_t *errcell) {
    block_t *top = heap_last;
    if (top && top->state == BLOCK_FREE) {
        const size_t lack = top->size >= size ? 0 : size - top->size;
        if (lack > heap_max - heap_current) {
            *errcell = ERR_HEAP_EXHAUSTED;
            return NULL;
        }
        if (list_remove(top)) {
            heap_current += lack;
            top->size    += lack;
            top->state    = BLOCK_USED;
            top->tag      = tag;
            cut(top, size);
            return payload_of(top);
        }
    }
    if (BLOCK_HDR_SIZE + size > heap_max - heap_current) {
        *errcell = ERR_HEAP_EXHAUSTED;
        return NULL;
    }
    block_t *b = (block_t *)heap_current;
    heap_current += BLOCK_HDR_SIZE + size;
    b->size  = size;
    b->magic = HEAP_MAGIC;
    b->state = BLOCK_USED;
    b->prev  = top;
    b->tag   = tag;
    heap_last = b;
    return payload_of(b);
}

/* A block of 2 MiB or more from the heap's end, over a range of whole 2 MiB
 * pages none of whose bytes is touched — the kernel cannot lay a large page
 * over one already there — with its header just below the range. What lies
 * between the heap's end and that header is the free block that ends the
 * heap grown over it, or a free block of its own; or, under 48 bytes, the
 * front of the block's payload, its header at the heap's end. */
static void *grow_huge(size_t size, uint8_t tag, error_t *errcell,
                       uintptr_t *huge_va, size_t *huge_len) {
    const uintptr_t from  = heap_current;
    const uintptr_t start = align_up(from + BLOCK_HDR_SIZE, HEAP_HUGE);
    const size_t    lead  = start - (from + BLOCK_HDR_SIZE);
    const size_t    span  = align_up(size, HEAP_HUGE);
    if (start > heap_max || span > heap_max - start) {
        *errcell = ERR_HEAP_EXHAUSTED;
        return NULL;
    }

    block_t *below = heap_last;
    block_t *h     = (block_t *)from;
    if (lead >= HEAP_MIN_SPLIT) {
        h = (block_t *)(start - BLOCK_HDR_SIZE);
        if (below && below->state == BLOCK_FREE && list_remove(below)) {
            below->size += lead;
            list_insert(below);
        } else {
            block_t *pad = (block_t *)from;
            pad->size  = lead - BLOCK_HDR_SIZE;
            pad->magic = HEAP_MAGIC;
            pad->state = BLOCK_FREE;
            pad->prev  = below;
            pad->tag   = HEAP_TAG_NONE;
            list_insert(pad);
            below = pad;
        }
    }
    h->size  = start + span - (uintptr_t)payload_of(h);
    h->magic = HEAP_MAGIC;
    h->state = BLOCK_USED;
    h->prev  = below;
    h->tag   = tag;
    heap_last    = h;
    heap_current = start + span;
    if (huge_va)  *huge_va  = start;
    if (huge_len) *huge_len = span;
    return payload_of(h);
}

void *heap_blocks_take(size_t size, uint8_t tag, error_t *errcell,
                       uintptr_t *huge_va, size_t *huge_len) {
    if (!initialized) heap_init();
    if (huge_len) *huge_len = 0;

    const size_t want = size == 0 ? HEAP_ALIGN : align_up(size, HEAP_ALIGN);
    if (want < size) {
        *errcell = ERR_NO_MEMORY;
        return NULL;
    }
    if (want > heap_max - heap_base) {
        *errcell = ERR_HEAP_EXHAUSTED;
        return NULL;
    }

    for (;;) {
        unsigned fl, sl;
        block_t *b = find_free(want, &fl, &sl);
        if (!b) break;
        if (b->magic != HEAP_MAGIC || b->state != BLOCK_FREE || !list_remove(b)) {
            list_drop(fl, sl);
            continue;
        }
        b->state = BLOCK_USED;
        b->tag   = tag;
        cut(b, want);
        *errcell = OK;
        return payload_of(b);
    }

    void *p = BLOCK_HDR_SIZE + want >= HEAP_HUGE ? grow_huge(want, tag, errcell, huge_va, huge_len)
                                                 : grow(want, tag, errcell);
    if (p) *errcell = OK;
    return p;
}

bool heap_blocks_resize(block_t *b, size_t size) {
    const size_t want = size == 0 ? HEAP_ALIGN : align_up(size, HEAP_ALIGN);
    if (want < size) return false;
    if (b->size >= want) {
        cut(b, want);
        return true;
    }
    error_t  seen  = OK;
    block_t *above = above_of(b, &seen);
    if (above) {
        if (above->state != BLOCK_FREE || b->size + BLOCK_HDR_SIZE + above->size < want) return false;
        block_t *beyond = above_of(above, &seen);
        if (seen != OK || !list_remove(above)) return false;
        join(b, above, beyond);
        cut(b, want);
        return true;
    }
    /* The last block grows with the heap's end, while it is no large one: a
     * block of 2 MiB or more is laid on large pages of its own. */
    if (seen != OK || BLOCK_HDR_SIZE + want >= HEAP_HUGE || want - b->size > heap_max - heap_current)
        return false;
    heap_current += want - b->size;
    b->size = want;
    return true;
}

void *heap_blocks_align(block_t *b, size_t alignment, size_t size) {
    const uintptr_t at = (uintptr_t)payload_of(b);
    if (at & (alignment - 1)) {
        error_t  seen   = OK;
        block_t *beyond = above_of(b, &seen);
        if (seen != OK) {
            (void)heap_blocks_give(b);
            return NULL;
        }
        const uintptr_t end    = (uintptr_t)block_after(b);
        const uintptr_t target = align_up(at + HEAP_MIN_SPLIT, alignment);
        block_t        *n      = block_of((void *)target);
        n->size  = end - target;
        n->magic = HEAP_MAGIC;
        n->state = BLOCK_USED;
        n->prev  = b;
        n->tag   = b->tag;
        if (beyond) beyond->prev = n;
        else        heap_last = n;
        b->size = (uintptr_t)n - at;
        (void)heap_blocks_give(b);
        b = n;
    }
    cut(b, align_up(size, HEAP_ALIGN));
    return payload_of(b);
}

void heap_blocks_bar(block_t *b) {
    b->state = BLOCK_FOREIGN;
    b->tag   = HEAP_TAG_NONE;
}

block_t *heap_blocks_first(void) {
    if (!initialized || heap_current == heap_base) return NULL;
    return (block_t *)heap_base;
}

block_t *heap_blocks_next(block_t *b) {
    if (b == heap_last) return NULL;
    block_t *n = block_after(b);
    if ((uintptr_t)n > heap_current - BLOCK_HDR_SIZE) return NULL;
    return n;
}

size_t heap_blocks_grown(void) {
    return initialized ? heap_current - heap_base : 0;
}
