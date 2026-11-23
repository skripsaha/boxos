#include "read.h"
#include "../run/run.h"   /* TagFSRunBlocks — how much the medium takes in one command */
#include "tagfs_volume.h"

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
    uint64_t                first;         /* the first block held */
    uint32_t                held;          /* blocks held from `first` on */
    uint64_t                coming_first;  /* the run on its way in ... */
    uint32_t                coming;        /* ... and its blocks; 0 for none */
    uint64_t                gen;           /* moves when a write lands on the run coming; never 0 */
    struct ReadAheadWindow *prev;
    struct ReadAheadWindow *next;
};


static bool WindowHolds(uint64_t first, uint32_t count, uint64_t block)
{
    return count > 0 && block >= first && block - first < count;
}

/* The handle's window, made at its first fetch as deep as the seat's run:
 * NULL when the heap has no room for it, and the read goes on a block at a
 * time. */
static struct ReadAheadWindow *WindowOf(TagFSVolume* vol, TagFSFileHandle *h)
{
    if (h->ahead)
        return h->ahead;

    uint32_t                room = TagFSRunBlocks(vol);
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

    TagFSLockTake(&vol->read_ahead.windows_lock);
    w->next = vol->read_ahead.windows;
    if (vol->read_ahead.windows)
        vol->read_ahead.windows->prev = w;
    vol->read_ahead.windows = w;
    TagFSLockDrop(&vol->read_ahead.windows_lock);
    h->ahead = w;
    return w;
}

const uint8_t *ReadAheadServe(TagFSVolume* vol, TagFSFileHandle *h, uint64_t block)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return NULL;
    TagFSLockTake(&vol->read_ahead.windows_lock);
    bool     held  = WindowHolds(w->first, w->held, block);
    uint64_t index = block - w->first;
    TagFSLockDrop(&vol->read_ahead.windows_lock);
    return held ? w->data + (size_t)index * TAGFS_BLOCK_SIZE : NULL;
}

uint64_t ReadAheadBegin(TagFSVolume* vol, TagFSFileHandle *h, uint64_t first, uint32_t *count)
{
    struct ReadAheadWindow *w = WindowOf(vol, h);
    if (!w)
        return 0;
    if (*count > w->room)
        *count = w->room;

    TagFSLockTake(&vol->read_ahead.windows_lock);
    w->held         = 0;           /* its buffer is the fetch's now */
    w->coming_first = first;
    w->coming       = *count;
    uint64_t gen    = w->gen;
    TagFSLockDrop(&vol->read_ahead.windows_lock);
    return gen;
}

uint8_t *ReadAheadBuffer(TagFSFileHandle *h)
{
    return h->ahead ? h->ahead->data : NULL;
}

bool ReadAheadLand(TagFSVolume* vol, TagFSFileHandle *h, uint64_t gen, uint32_t fetched)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return false;
    TagFSLockTake(&vol->read_ahead.windows_lock);
    bool kept = fetched > 0 && w->gen == gen && fetched <= w->coming;
    if (kept) {
        w->first = w->coming_first;
        w->held  = fetched;
    }
    w->coming = 0;
    TagFSLockDrop(&vol->read_ahead.windows_lock);
    return kept;
}

void ReadAheadFetch(TagFSVolume* vol, TagFSFileHandle *h, uint64_t first, uint32_t count)
{
    uint64_t gen = ReadAheadBegin(vol, h, first, &count);
    if (gen == 0 || count == 0)
        return;
    bool got = TagFSReadRun(vol, first, count, ReadAheadBuffer(h)) == OK;
    ReadAheadLand(vol, h, gen, got ? count : 0);
}

void ReadAheadLetGo(TagFSVolume* vol, TagFSFileHandle *h)
{
    struct ReadAheadWindow *w = h->ahead;
    if (!w)
        return;
    h->ahead = NULL;

    TagFSLockTake(&vol->read_ahead.windows_lock);
    if (w->prev)
        w->prev->next = w->next;
    else
        vol->read_ahead.windows = w->next;
    if (w->next)
        w->next->prev = w->prev;
    TagFSLockDrop(&vol->read_ahead.windows_lock);

    kfree(w->data);
    kfree(w);
}

/* The blocks a window holds before the written one are as they were; the
 * written one and those after it are let go of. A fetch on its way over it
 * read the medium before the write landed, and is dropped when it lands. */
void ReadAheadForgetBlock(TagFSVolume* vol, uint64_t block)
{
    TagFSLockTake(&vol->read_ahead.windows_lock);
    for (struct ReadAheadWindow *w = vol->read_ahead.windows; w; w = w->next) {
        if (WindowHolds(w->first, w->held, block))
            w->held = (uint32_t)(block - w->first);
        if (WindowHolds(w->coming_first, w->coming, block))
            w->gen++;
    }
    TagFSLockDrop(&vol->read_ahead.windows_lock);
}

void ReadAheadForgetAll(TagFSVolume* vol)
{
    TagFSLockTake(&vol->read_ahead.windows_lock);
    for (struct ReadAheadWindow *w = vol->read_ahead.windows; w; w = w->next) {
        w->held = 0;
        w->gen++;
    }
    TagFSLockDrop(&vol->read_ahead.windows_lock);
}

/* The volume's read_ahead part as it begins: what was not nothing. */
void ReadAheadVolumeBegin(TagFSVolume* vol)
{
    vol->read_ahead.windows_lock = (TagFSLock)TAGFS_LOCK_INIT("readahead", LOCK_RANK_LEAF);
}
