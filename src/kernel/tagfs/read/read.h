#ifndef TAGFS_READ_H
#define TAGFS_READ_H

#include "../tagfs.h"
#include "../file_map/file_record.h"
#include "vmm.h"

/*
 * The read path: where a read's bytes go (the sink), the window of blocks a
 * handle reading in order has fetched ahead of itself (read_ahead.c), and the
 * stream from a handle's offset into a sink (read.c). Both roads a program's
 * read takes — the one that stands in its call and the one that parks it
 * (storage/async/read_job.c) — find a file's blocks and answer a hole the same
 * way, here.
 */

/* Where a read's bytes go: the kernel's own memory, or a program's pages
 * written a piece at a time as the read reaches them — never the whole read
 * bounced through the heap first. A page of the program's is written as its
 * own first touch would find it: brought in if it is heap it has not been
 * through yet, refused if the program holds it read-only. */
typedef struct ReadSink {
    uint8_t       *kernel;   /* the bytes' place, when it is the kernel's own */
    vmm_context_t *vmm;      /* else the program's pages ... */
    uintptr_t      user;     /* ... from this address */
    uint64_t       bytes;    /* the room */
} ReadSink;

/* `bytes` of `src` into the sink from `at` on: OK, ERR_INVALID_ARGUMENT past
 * its room, or what the program's pages answer (ERR_INVALID_ADDRESS,
 * ERR_NO_MEMORY). */
error_t ReadSinkPush(const ReadSink *sink, uint64_t at, const void *src, uint32_t bytes);

/* `bytes` zeros into the sink from `at` on — what a hole of a file reads as —
 * answered as ReadSinkPush answers. */
error_t ReadSinkZeros(const ReadSink *sink, uint64_t at, uint64_t bytes);

/* Block k of the handle's file — found in the record it holds, down its Tract
 * (file_map/file_record.h), never by walking every run from the first — and
 * how many of the file's next blocks follow it in the same Strip. A hole is a
 * Strip the record names (STRIP_HOLE): it reads as zeros, and no block of the
 * medium is asked. A block inside the file's size that no Strip covers is not
 * a hole: it is a record that describes more file than there are blocks, said
 * with the numbers and answered ERR_METADATA_CORRUPTED (S2). */
error_t ReadLocate(const TagFSFileHandle *h, uint64_t k, FileSpot *out);

/*
 * A handle's read-ahead window: as many blocks as the medium hands over in one
 * command (BoardroomSeatRun), fetched in that one command when a read in order
 * reaches a block the window does not hold. It is the handle's — allocated at
 * its first fetch, let go of at its close — so a medium that takes more at a
 * time is asked for more, and two files read side by side never push each
 * other's blocks out.
 *
 * ‼ A WRITE THAT LANDS IS NEVER UNDONE BY A FETCH (N1). Every window of the
 * machine is on one list a write walks (ReadAheadForgetBlock): a block it
 * holds is let go of, and a window whose fetch is on its way over that block
 * moves its generation, so the fetch — which read the medium before the write
 * landed — is dropped when it lands instead of filling the window with what
 * the block held before.
 */
struct ReadAheadWindow;

/* The block, when the handle's window holds it: its bytes, which stay the
 * window's until the handle's next fetch. NULL when it does not. */
const uint8_t *ReadAheadServe(TagFSFileHandle *h, uint64_t block);

/* The handle's next `count` blocks from `first` on, fetched into its window in
 * one command — as many of them as the window holds. Nothing is said when the
 * window cannot be had or the medium refuses: the read goes on block by block,
 * and a block that will not come is that read's answer. */
void    ReadAheadFetch(TagFSFileHandle *h, uint64_t first, uint32_t count);

/* The handle's window let go of (tagfs_close). */
void    ReadAheadLetGo(TagFSFileHandle *h);

/* A write landed on `block` (tagfs_readahead_invalidate). */
void    ReadAheadForgetBlock(uint64_t block);

/* Every window emptied: what they hold are copies of a medium that left. */
void    ReadAheadForgetAll(void);

/* A fetch in its two halves, for ReadAheadFetch and for the stand that proves
 * a late fetch is dropped: Begin makes the handle's window (when it has none)
 * and gives up what it held, names the run on its way in — *count cut to the
 * window's room — and answers the generation it was named under, 0 when the
 * window could not be had; Land keeps the `fetched` blocks now in its buffer
 * only when no write has landed on the run since Begin, and answers whether it
 * kept them. Between the two the run's bytes are read into ReadAheadBuffer(h). */
uint64_t ReadAheadBegin(TagFSFileHandle *h, uint64_t first, uint32_t *count);
uint8_t *ReadAheadBuffer(TagFSFileHandle *h);
bool     ReadAheadLand(TagFSFileHandle *h, uint64_t gen, uint32_t fetched);

/* The read of a handle into a sink (tagfs_read, TagFSReadInto), inside the
 * volume's door: see tagfs.h. */
error_t TagFSReadStream(TagFSFileHandle *h, const ReadSink *sink, uint64_t *out_read);

#endif /* TAGFS_READ_H */
