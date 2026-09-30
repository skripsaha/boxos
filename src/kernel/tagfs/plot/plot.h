#ifndef TAGFS_PLOT_H
#define TAGFS_PLOT_H

#include "../tagfs.h"
#include "vmm.h"

/*
 * Plot — a file's extents as one call works on them, and all of their algebra.
 *
 * Every write, span and defrag loads the file's record into a Plot under the
 * file's Helm (PlotLoad), works on the Plot in memory — finds blocks, takes
 * new ground, merges runs, checks the record still fits — and inks the one
 * record the Plot builds (TagFSInkPlot, ink/). What the call took and did not
 * ink is given back on its way out, whichever way out it takes (PlotRelease):
 * a block is a Stake from the moment it is taken until an inked record names
 * it, and a Stake nobody named goes back.
 *
 * The widths of format v1 are known here and in plot/plot_width.h only: the
 * record's 42 + 2T + 6E + N <= 4080 (PlotFits) and an extent's 0xFFFF blocks
 * (PlotAppendRun cuts a run at it). Every count in memory is u32; PackV1 is
 * where a record meets the medium's widths, and it refuses one that would not
 * fit rather than cut it.
 *
 * A write grows a file at its end or not at all (PlotBeginWrite): an offset
 * past the size is refused ERR_TAGFS_BEYOND_SIZE. A gap is made only by span,
 * and it reads as zeros (PlotExtendZero).
 */

/* A run of blocks this call took and no committed record names yet. */
typedef struct {
    uint32_t start;
    uint32_t count;
} Stake;

typedef struct {
    uint32_t      fid;
    uint32_t      mount_epoch;    /* the mounting the Plot's block numbers are of */
    TagFSMetadata meta;           /* the record as loaded: name, tags, flags, times */
    uint64_t      shape;          /* the Ink that gave the loaded record its size and extents */
    uint64_t      data_need;      /* the flush need the blocks this call wrote hold, 0 for none */
    uint64_t      data_us;        /* the medium's time on this call's data, measured, since its last Ink */
    uint64_t      size_before;    /* the size the call found; what existed is below it */
    uint64_t      size;           /* the size the call leaves */
    FileExtent   *runs;           /* the working extents, in file order */
    uint32_t      run_count;
    uint32_t      run_room;
    uint64_t      blocks;         /* how many blocks the runs name */
    uint64_t      blocks_before;  /* ... and how many the loaded record named */
    uint32_t      runs_before;    /* ... in how many extents */
    Stake        *stakes;
    uint32_t      stake_count;
    uint32_t      stake_room;
    FileExtent   *freed;          /* ground the record stops naming, to Fallow once it is inked */
    uint32_t      freed_count;
    uint32_t      freed_room;
    FileHelm     *helm;           /* the Helm its holder loaded it under; NULL for none */
    uint32_t      claimed;        /* the extents it claimed room for in the record (RT-19) */
} Plot;

/* The file's record, out of the map of live files afresh — never a handle's
 * copy of it. OK, or ERR_FILE_NOT_FOUND, ERR_METADATA_CORRUPTED (the record
 * names fewer blocks than its size needs), the refusal of the one guard of a
 * change (InkMayChange: ERR_PERMISSION_DENIED for a file the loader reads by
 * its blocks, ERR_DEVICE_NOT_READY once the medium left) — asked here, before
 * a byte of the call moves — or ERR_NO_MEMORY. A holder of the file's Helm
 * sets `helm` after the load, so the record's room is claimed for it
 * (PlotReserve). */
error_t  PlotLoad(Plot *p, uint32_t fid);

/* Everything the Plot holds let go; every Stake still standing is given back
 * first (PlotPullStakes), and the room it claimed (InkRoomLetGo). Safe on a
 * Plot PlotLoad refused. */
void     PlotRelease(Plot *p);

/* Every Stake given back to the allocator: the way out of a call that commits
 * nothing, or commits less than it took. A Plot of an earlier mounting gives
 * nothing back — its numbers name another volume's blocks now, and the ones it
 * took are the roll call's to find. */
void     PlotPullStakes(Plot *p);

/* Where block k of a file lies: true, *out_block, and in *out_left how many
 * of the run's blocks follow it. False when the runs do not reach block k. */
bool     PlotFind(const FileExtent *runs, uint32_t count, uint64_t k,
                  uint32_t *out_block, uint32_t *out_left);

/* Whether block k held the file's bytes before this call: k < ceil(size/4096)
 * of the size the call found. A block the file names past that holds nothing
 * of it, whoever had the ground before. */
bool     PlotExisted(const Plot *p, uint64_t k);

/* The one read-modify-write rule, for every road a block is written by: what a
 * write of `bytes` at `offset` inside block k does with the rest of the block. */
typedef enum {
    PLOT_FILL_WHOLE = 0,   /* the write covers the block: nothing to keep, nothing read */
    PLOT_FILL_FRESH,       /* the block held nothing of the file: zeros, never read */
    PLOT_FILL_READ,        /* it holds the file's bytes: read, and a refused read stops the write */
} PlotFill;

PlotFill PlotFillFor(const Plot *p, uint64_t k, uint32_t offset, uint32_t bytes);

/* The run [start, start + count) after the Plot's last one: merged into it
 * when it begins where that one ends, and cut into extents of at most 0xFFFF
 * blocks. OK or ERR_NO_MEMORY, the runs unchanged. */
error_t  PlotAppendRun(Plot *p, uint32_t start, uint32_t count);

/* Whether a record fits format v1: its length in *out_bytes when it does. */
bool     PlotFits(const TagFSMetadata *record, uint32_t *out_bytes);

/* Ground under every block of [0, end) bytes: what the file lacks is taken
 * through its Allotment near its last block, each run a Stake. Then the
 * record must fit: an extent list too long is shortened by PlotGather, and
 * ERR_TAGFS_RECORD_FULL — said, with its numbers — when it cannot be; and the
 * room its extents need is claimed (InkRoomClaim). Nothing is written but what
 * a gather moves. */
error_t  PlotReserve(Plot *p, uint64_t end);

/* The growth rule, and the ground: where a write of `bytes` at `offset` begins
 * (at the size when `append`), ERR_TAGFS_BEYOND_SIZE past the size, and
 * PlotReserve for everything it covers. Before any byte of the write moves. */
error_t  PlotBeginWrite(Plot *p, uint64_t offset, bool append, uint64_t bytes,
                        uint64_t *out_at);

/* The write that began at `at` made ready to be the file's for the `done`
 * bytes of it that landed: the ground reserved past them cut off (CR-13) and
 * the size grown to their end. The caller inks the record (TagFSInkPlot). */
error_t  PlotEndWrite(Plot *p, uint64_t at, uint64_t done);

/* The runs cut to their first keep_blocks blocks. Staked ground cut off goes
 * back now; ground a record named comes to rest in Fallow once a record that
 * no longer names it is inked. */
error_t  PlotTrimTo(Plot *p, uint64_t keep_blocks);

/* The file grown to `size` with zeros: the old last block's bytes past the old
 * size zeroed (read first — a refused read refuses the span), and every block
 * after it taken and written as zeros. The Plot's size is `size` after OK. */
error_t  PlotExtendZero(Plot *p, uint64_t size);

/*
 * PlotGather — extents made fewer by moving the fewest blocks.
 *
 * The consecutive extents whose blocks, laid in one run, shed at least `shed`
 * extents from the record, the window with the fewest blocks of all such: one
 * run of exactly that many is taken near `hint`, the blocks are copied into it
 * and flushed, and the Plot names the run instead. The ground left comes to
 * rest once a record that no longer names it is inked. `shed` 0 asks for
 * every extent of the file in one run (defrag).
 *
 * Only ground the loaded record named is moved, never a Stake — and never a
 * file the loader reads by its blocks: PlotLoad refused that one. Refused —
 * said, with the numbers — when no window sheds enough or no run that long is
 * free.
 */
error_t  PlotGather(Plot *p, uint32_t shed, uint32_t hint);

/*
 * Where a write's bytes come from: the kernel's own memory, or a program's
 * pages read in pieces as the write reaches them — never the whole write
 * copied into the heap first.
 *
 * A pull reads only pages the program has: it never brings one in, because a
 * write pulls where its job happens to run — a continuation on the drain core,
 * or a service point — which is not the program's own context, while bringing
 * a page in is the program's own first touch. So the pages are brought in
 * first, in the program's call, before the job is built (PlotSourceBringIn).
 */
typedef struct PlotSource {
    const uint8_t *kernel;   /* the bytes, when they are the kernel's own */
    vmm_context_t *vmm;      /* else the program's pages ... */
    uintptr_t      user;     /* ... from this address */
    uint64_t       bytes;
} PlotSource;

/* Every page of the source the program has not touched yet brought in, as its
 * own first touch would bring it: OK, or ERR_NO_MEMORY / ERR_INVALID_ADDRESS
 * at the first page that could not be had. Called with no TagFS lock held. */
error_t  PlotSourceBringIn(const PlotSource *src);

/* `bytes` of the source from `at` into dst: OK, or ERR_INVALID_ADDRESS when a
 * page is not there — the program gave it up since it was brought in. */
error_t  PlotSourcePull(const PlotSource *src, uint64_t at, void *dst, uint32_t bytes);

/* The source written at `at` block by block, synchronously, each block by the
 * one read-modify-write rule. It stops at the first refusal — a read, the
 * source's pages, a write — and *out_done is what landed before it. */
error_t  PlotWriteBlocks(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done);

/* A record packed for the medium: its length, or 0 — said — when it is wider
 * than format v1 carries or names a block outside the data run. */
uint32_t PackV1(const TagFSMetadata *meta, uint8_t *out);

/* A packed record given the file id `fid`, and summed again. */
void     PackV1Refile(uint8_t *rec, uint32_t fid);

/* A packed record read back, whose room in its block is `room` bytes: OK,
 * ERR_METADATA_CORRUPTED when it does not check out, or ERR_NO_MEMORY. */
error_t  UnpackV1(const uint8_t *rec, uint32_t room, TagFSMetadata *out);

#endif /* TAGFS_PLOT_H */
