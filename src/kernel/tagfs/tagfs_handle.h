#ifndef TAGFS_HANDLE_H
#define TAGFS_HANDLE_H

// ============================================================================
// TagFS In-Memory Structures: File Handles and Metadata
// A file's Helm and who waits for it, the open-file table's row, and the
// handle one operation holds. tagfs.h includes it; nothing here needs more
// than the lock and the Strip.
// ============================================================================

#include "../../lib/kernel/ktypes.h"
#include "lock_rank/lock_rank.h"
#include "plot/strip.h"

/* A place in a file's Helm (helm/helm.h): the FIFO while it waits, then the
 * ready list when its work is run at a service point. */
typedef struct HelmWaiter {
    struct HelmWaiter* next;
    void (*hand)(struct HelmWaiter* w);   /* the Helm is w's now; no TagFS lock held */
    void (*run)(struct HelmWaiter* w);    /* a waiter HelmReady listed is run here */
    /* The medium left: w is answered here instead of ever being handed the
     * Helm — asked while it was away, or waiting when its open book was given
     * up (mount/away.h) — with the count of changes no cycle settled that the
     * book gave up; 0 for one asked while the book was still held. No TagFS
     * lock held. */
    void (*gone)(struct HelmWaiter* w, uint64_t lost);
} HelmWaiter;

/* One holder of a file's extents at a time, and who waits for it. */
typedef struct {
    TagFSLock     lock;       /* leaf: the four below */
    HelmWaiter*   holder;     /* NULL when nobody holds it */
    HelmWaiter*   first;      /* the FIFO, oldest first */
    HelmWaiter*   last;
    /* Its holder let go while the medium was away: the FIFO is held with the
     * volume's open book, handed on nobody until the book is taken up. */
    bool          parked;
    /* A turn of Engross found it held (HelmTryNoted): the holder's let-go asks
     * the service again. */
    bool          noted;
    volatile bool vacated;    /* a Quitclaim gave the file up */
} FileHelm;

struct TagFSFileHandle;

/*
 * Whether a change carries the warrant to open a file the system keeps — one
 * whose record puts it in a role — system, boot or kernel (ink.h, the one
 * guard). The
 * deck that takes a program's call sets it, from the authority the program's
 * cabin holds: system or god warrants, nothing else does. It rides to the
 * guard in the handle and in the job, and in the calls the deck makes itself
 * (a delete). A caller in the kernel carries none. No
 * warrant reaches the blocks the Deed points the loader at.
 */
typedef enum {
    INK_UNWARRANTED = 0,
    INK_WARRANTED   = 1,
} InkWarrant;

/* A file some handle has open: its Helm, and every open handle's view of it,
 * under the open-file table's lock (helm/helm.c). */
typedef struct OpenFileEntry {
    uint64_t                file_id;
    uint32_t                ref_count;
    FileHelm                helm;
    struct TagFSFileHandle* views;
    struct OpenFileEntry*   next;
} OpenFileEntry;

/* A file opened for one operation. A handle opened to read holds the record
 * as the open found it — a reference on it (file_map/file_record.h), never a
 * copy of its runs — and its `file_size`, and only a read walks them; one
 * opened only to write holds no record (NULL): its work loads the record under
 * the file's Helm (plot.h, PlotLoad) and leaves here only where it ended —
 * `offset`, and the size it left. `view_gen` is the newest Ink when the open
 * looked: ground an Ink after it frees from the file rests until the handle is
 * closed (helm.h). `warrant` is the caller's (InkWarrant): what the handle's
 * work may change, set by whoever opened it for a program, none from the open. */
typedef struct TagFSFileHandle {
    uint64_t    file_id;
    uint32_t    flags;
    InkWarrant  warrant;
    uint64_t    offset;
    uint64_t    file_size;
    struct FileRecord* rec;
    OpenFileEntry* ofe;

    /* Which mounting of the volume the record above was taken from.
     *
     * A medium that leaves and comes back is READ AGAIN, so the block numbers
     * a handle is carrying describe the volume as it was before it left. On
     * the volume as it is now they are somebody else's blocks, and following
     * them is not a failed read — it is a successful read of the wrong file.
     * See tagfs_mount_epoch(). */
    uint32_t    mount_epoch;

    uint64_t                view_gen;
    struct TagFSFileHandle* view_prev;   /* the file's views (OpenFileEntry.views) */
    struct TagFSFileHandle* view_next;

    /* The blocks a read in order has fetched ahead of itself, as many as the
     * medium hands over in one command (read/read.h); NULL until the first
     * fetch, and let go of when the handle is closed. */
    struct ReadAheadWindow* ahead;
} TagFSFileHandle;

typedef struct {
    uint64_t    file_id;
    uint32_t    flags;
    uint64_t    size;
    uint64_t    created_time;
    uint64_t    modified_time;
    uint64_t    keep_until;      /* with TAGFS_FILE_DELETED: when the volume lets the file go */
    char*       filename;
    uint32_t*   tag_ids;
    uint32_t    tag_count;
    Strip*      extents;
    uint32_t    extent_count;
} TagFSMetadata;

#endif // TAGFS_HANDLE_H
