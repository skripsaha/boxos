#include "binder.h"

/*
 * The births of the runs this mount's writes put in (binder.h): the ground a
 * settled stitch named as fresh, by its first block, with that stitch's seq —
 * what a Tally writes as each such run's birth (§2.9 birth_delta).
 *
 * ‼ SORTED RUNS IN CHUNKS, NO TREE. The entries never overlap and are kept in
 * order of their first block, a chunk of them a page, the chunks in an array
 * in the same order: a lookup halves the array and then the chunk, a stitch's
 * runs go in where they belong, and a chunk that fills is cut in two. The
 * ground a stitch names is ground no live run names (a fresh block is never
 * one a settled record holds, §3.1), so what it overlaps is only what an
 * earlier owner left behind — cut out as the new runs go in.
 *
 * ‼ WHAT GOES BACK GOES OUT UNDER THE STATE LOCK, WITH NOTHING ASKED OF THE
 * HEAP AND NOTHING GIVEN IT. Fallow's gives run there (fallow.c, NodeGiveBack),
 * so an entry the ground covers whole goes out and one it reaches into is cut
 * short at that end; ground taken out of the middle of an entry leaves the
 * entry whole — a write cut a run of it in two, and both halves still name it
 * — and the stitch that next names any of that ground as its own cuts the
 * entry where the heap can be asked. A chunk left empty leaves the array (a
 * lookup reads each chunk's first run) and is set aside, and BirthsTrim gives
 * it to the heap once the lock is let go — the TagFS lock is an interrupts-off
 * spinlock (lock_rank.h), and a give back of every Tally's worth of ground
 * empties chunks by the hundred.
 */

#define BIRTHS_CHUNK  168u          /* 24 bytes an entry: a chunk is a page */

typedef struct {
    uint64_t start;
    uint64_t count;
    uint64_t seq;
} BirthRun;

typedef struct BirthChunk BirthChunk;
struct BirthChunk {
    uint32_t    n;
    uint32_t    pad;
    BirthChunk* spent;              /* set aside empty, for BirthsTrim */
    BirthRun    run[BIRTHS_CHUNK];
};

static BirthChunk** s_chunk;        /* in the order of their first runs */
static uint32_t     s_chunks;
static uint32_t     s_room;
static BirthChunk*  s_spent;        /* emptied under the state lock, not yet given the heap */

/* The chunk the run holding `block` would lie in: the last whose first run
 * begins at or before it — 0 when none does, or there are none. */
static uint32_t BirthsChunkOf(uint64_t block)
{
    uint32_t lo = 0, hi = s_chunks;
    while (hi - lo > 1u) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (s_chunk[mid]->run[0].start <= block)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* In chunk `c`, the first run that ends past `block`. */
static uint32_t BirthsRunPast(const BirthChunk* c, uint64_t block)
{
    uint32_t lo = 0, hi = c->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (c->run[mid].start + c->run[mid].count <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

uint64_t BirthsOf(uint64_t block)
{
    if (s_chunks == 0)
        return 0;
    const BirthChunk* c = s_chunk[BirthsChunkOf(block)];
    uint32_t          i = BirthsRunPast(c, block);
    return i < c->n && c->run[i].start <= block ? c->run[i].seq : 0;
}

/* Run `i` of chunk `ci` out of it; the chunk out of the array once empty,
 * and set aside for BirthsTrim. */
static void BirthsDrop(uint32_t ci, uint32_t i)
{
    BirthChunk* c = s_chunk[ci];
    memmove(&c->run[i], &c->run[i + 1u], sizeof(BirthRun) * (c->n - i - 1u));
    if (--c->n)
        return;
    memmove(&s_chunk[ci], &s_chunk[ci + 1u], sizeof(BirthChunk*) * (s_chunks - ci - 1u));
    s_chunks--;
    c->spent = s_spent;
    s_spent  = c;
}

void BirthsTrim(void)
{
    while (s_spent) {
        BirthChunk* next = s_spent->spent;
        kfree(s_spent);
        s_spent = next;
    }
}

void BirthsForget(uint64_t start, uint64_t count)
{
    uint64_t end = start + count;
    for (uint32_t ci = s_chunks ? BirthsChunkOf(start) : 0; ci < s_chunks;) {
        BirthChunk* c = s_chunk[ci];
        uint32_t    i = BirthsRunPast(c, start);
        if (i == c->n) {
            ci++;
            continue;
        }
        BirthRun* r = &c->run[i];
        uint64_t  e = r->start + r->count;
        if (r->start >= end)
            return;
        if (r->start >= start && e <= end) {
            BirthsDrop(ci, i);
            continue;
        }
        if (r->start >= start) {            /* the ground reaches into its front */
            r->count = e - end;
            r->start = end;
            return;
        }
        if (e <= end)                       /* ... into its back */
            r->count = start - r->start;
        return;                             /* out of its middle: both halves still name it */
    }
}

/* Room for one run more in chunk `ci` at `i`: the chunk cut in two when full.
 * The chunk and the place it lands in, or false with no memory. */
static bool BirthsRoomAt(uint32_t* ci, uint32_t* i)
{
    BirthChunk* c = s_chunk[*ci];
    if (c->n < BIRTHS_CHUNK)
        return true;
    if (s_chunks == s_room) {
        uint32_t     room = s_room ? s_room * 2u : 16u;
        BirthChunk** more = kmalloc(sizeof(BirthChunk*) * room);
        if (!more)
            return false;
        memcpy(more, s_chunk, sizeof(BirthChunk*) * s_chunks);
        kfree(s_chunk);
        s_chunk = more;
        s_room  = room;
    }
    BirthChunk* half = kmalloc(sizeof(BirthChunk));
    if (!half)
        return false;
    half->spent = NULL;
    half->n     = c->n / 2u;
    c->n   -= half->n;
    memcpy(half->run, &c->run[c->n], sizeof(BirthRun) * half->n);
    memmove(&s_chunk[*ci + 2u], &s_chunk[*ci + 1u], sizeof(BirthChunk*) * (s_chunks - *ci - 1u));
    s_chunk[*ci + 1u] = half;
    s_chunks++;
    if (*i > c->n) {
        *i -= c->n;
        (*ci)++;
    }
    return true;
}

/* `r` into the chunks at its place. False with no memory. */
static bool BirthsPut(const BirthRun* r)
{
    if (s_chunks == 0) {
        if (!s_chunk && (s_chunk = kmalloc(sizeof(BirthChunk*) * 16u)) != NULL)
            s_room = 16u;
        BirthChunk* c = s_chunk ? kmalloc(sizeof(BirthChunk)) : NULL;
        if (!c)
            return false;
        c->n       = 0;
        c->spent   = NULL;
        s_chunk[0] = c;
        s_chunks   = 1;
    }
    uint32_t ci = BirthsChunkOf(r->start);
    uint32_t i  = BirthsRunPast(s_chunk[ci], r->start);
    if (!BirthsRoomAt(&ci, &i))
        return false;
    BirthChunk* c = s_chunk[ci];
    memmove(&c->run[i + 1u], &c->run[i], sizeof(BirthRun) * (c->n - i));
    c->run[i] = *r;
    c->n++;
    return true;
}

/* What of [start, start + count) earlier runs name, cut out of them — a run it
 * lies in the middle of cut in two. False with no memory. */
static bool BirthsCut(uint64_t start, uint64_t count)
{
    uint64_t end = start + count;
    for (uint32_t ci = s_chunks ? BirthsChunkOf(start) : 0; ci < s_chunks;) {
        BirthChunk* c = s_chunk[ci];
        uint32_t    i = BirthsRunPast(c, start);
        if (i == c->n) {
            ci++;
            continue;
        }
        BirthRun r = c->run[i];
        if (r.start >= end)
            return true;
        if (r.start < start && r.start + r.count > end) {
            BirthRun back = { end, r.start + r.count - end, r.seq };
            c->run[i].count = start - r.start;
            return BirthsPut(&back);
        }
        BirthsForget(r.start < start ? start : r.start,
                     (r.start + r.count < end ? r.start + r.count : end) - (r.start < start ? start : r.start));
    }
    return true;
}

error_t BirthsNoted(const Strip* strips, uint64_t count, uint64_t seq)
{
    error_t rc = OK;
    for (uint64_t i = 0; rc == OK && i < count; i++) {
        if (strips[i].kind != STRIP_DATA)
            continue;
        BirthRun r = { strips[i].start, strips[i].count, seq };
        if (!BirthsCut(r.start, r.count) || !BirthsPut(&r))
            rc = ERR_NO_MEMORY;
    }
    BirthsTrim();
    return rc;
}

void BirthsLetGo(void)
{
    BirthsSharedLetGo();
    BirthsTrim();
    for (uint32_t i = 0; i < s_chunks; i++)
        kfree(s_chunk[i]);
    kfree(s_chunk);
    s_chunk  = NULL;
    s_chunks = 0;
    s_room   = 0;
}
