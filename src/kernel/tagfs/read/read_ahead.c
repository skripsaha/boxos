#include "read.h"
#include "../run/run.h"   /* TagFSRunBlocks — how much the medium takes in one command */

/*
 * The read-ahead windows (read.h). A window's bytes are its handle's alone —
 * only the handle's own read serves from them and fetches into them, and a
 * handle is one operation's — so they move with no lock held; the lock keeps
 * what a write has to see: which run a window holds, which one is on its way
 * in, and the generation a write moves.
 */
struct ReadAheadWindow {
    uint8_t                *data;          /* `room` blocks, from the heap */
    uint32_t                room;
    uint32_t                first;         /* the first block held */
    uint32_t                held;          /* blocks held from `first` on */
    uint32_t                coming_first;  /* the run on its way in ... */
    uint32_t                coming;        /* ... and its blocks; 0 for none */
    uint64_t                gen;           /* moves when a write lands on the run coming; never 0 */
    struct ReadAheadWindow *prev;
    struct ReadAheadWindow *next;
};

static struct ReadAheadWindow *g_windows;
static TagFSLock               g_windows_lock = TAGFS_LOCK_INIT("readahead", LOCK_RANK_LEAF);

static bool WindowHolds(uint32_t first, uint32_t count, uint32_t block)
{
    return count > 0 && block >= first && block - first < count;
}

/* The handle's window, made at its first fetch as deep as the seat's run:
 * NULL when the heap has no room for it, and the read goes on a block at a
 * time. */
static struct ReadAheadWindow *WindowOf(TagFSFileHandle *h)
{
    if (h->ahead)
        return h->ahead;

    uint32_t                room = TagFSRunBlocks();
    struct ReadAheadWindow *w    = kmalloc(sizeof(*w));
    uint8_t                *data = w ? kmalloc((size_t)room * TAGFS_BLOCK_SIZE) : NULL;
    if (!data) {
        kfree(w);
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->data = data;
    w->room = room;
    w->gen  = 1;

    TagFSLockTake(&g_windows_lock);
    w->next = g_windows;
    if (g_windows)
        g_windows->prev = w;
    g_windows = w;
    TagFSLockDrop(&g_windows_lock);
    h->ahead = w;
    return w;
}

const uint8_t *ReadAheadServe(TagFSFileHandle *h, uint32_t block)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return NULL;
    TagFSLockTake(&g_windows_lock);
    bool     held  = WindowHolds(w->first, w->held, block);
    uint32_t index = block - w->first;
    TagFSLockDrop(&g_windows_lock);
    return held ? w->data + (size_t)index * TAGFS_BLOCK_SIZE : NULL;
}

uint64_t ReadAheadBegin(TagFSFileHandle *h, uint32_t first, uint32_t *count)
{
    struct ReadAheadWindow *w = WindowOf(h);
    if (!w)
        return 0;
    if (*count > w->room)
        *count = w->room;

    TagFSLockTake(&g_windows_lock);
    w->held         = 0;           /* its buffer is the fetch's now */
    w->coming_first = first;
    w->coming       = *count;
    uint64_t gen    = w->gen;
    TagFSLockDrop(&g_windows_lock);
    return gen;
}

uint8_t *ReadAheadBuffer(TagFSFileHandle *h)
{
    return h->ahead ? h->ahead->data : NULL;
}

bool ReadAheadLand(TagFSFileHandle *h, uint64_t gen, uint32_t fetched)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return false;
    TagFSLockTake(&g_windows_lock);
    bool kept = fetched > 0 && w->gen == gen && fetched <= w->coming;
    if (kept) {
        w->first = w->coming_first;
        w->held  = fetched;
    }
    w->coming = 0;
    TagFSLockDrop(&g_windows_lock);
    return kept;
}

void ReadAheadFetch(TagFSFileHandle *h, uint32_t first, uint32_t count)
{
    uint64_t gen = ReadAheadBegin(h, first, &count);
    if (gen == 0 || count == 0)
        return;
    bool got = TagFSReadRun(first, count, ReadAheadBuffer(h)) == OK;
    ReadAheadLand(h, gen, got ? count : 0);
}

void ReadAheadLetGo(TagFSFileHandle *h)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return;
    h->ahead = NULL;

    TagFSLockTake(&g_windows_lock);
    if (w->prev)
        w->prev->next = w->next;
    else
        g_windows = w->next;
    if (w->next)
        w->next->prev = w->prev;
    TagFSLockDrop(&g_windows_lock);

    kfree(w->data);
    kfree(w);
}

/* The blocks a window holds before the written one are as they were; the
 * written one and those after it are let go of. A fetch on its way over it
 * read the medium before the write landed, and is dropped when it lands. */
void ReadAheadForgetBlock(uint32_t block)
{
    TagFSLockTake(&g_windows_lock);
    for (struct ReadAheadWindow *w = g_windows; w; w = w->next) {
        if (WindowHolds(w->first, w->held, block))
            w->held = block - w->first;
        if (WindowHolds(w->coming_first, w->coming, block))
            w->gen++;
    }
    TagFSLockDrop(&g_windows_lock);
}

void ReadAheadForgetAll(void)
{
    TagFSLockTake(&g_windows_lock);
    for (struct ReadAheadWindow *w = g_windows; w; w = w->next) {
        w->held = 0;
        w->gen++;
    }
    TagFSLockDrop(&g_windows_lock);
}
