#ifndef TAGFS_PLOT_H
#define TAGFS_PLOT_H

#include "../tagfs.h"
#include "../file_map/file_record.h"
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
 * A file's runs in memory are Strips (strip.h) of any length a u32 counts,
 * from any block a u64 names: nothing in the algebra below cuts a run to the
 * width a format gives it on the medium. The widths of format v1 are known in
 * plot/plot_width.h and plot_pack.c only — the record's 42 + 2T + 6E + N <=
 * 4080 (PlotFits), and an extent's u32 first block and 0xFFFF blocks, which
 * PackV1 cuts a long Strip to (PackV1Extents counts what that cut makes).
 * PackV1 is where a record meets the medium's widths, and it refuses one that
 * would not fit rather than cut it.
 *
 * A write grows a file at its end or not at all (PlotBeginWrite): an offset
 * past the size is refused ERR_TAGFS_BEYOND_SIZE. A gap is made only by span,
 * and it reads as zeros (PlotExtendZero).
 */

/* A run of blocks this call took and no committed record names yet. */
typedef struct {
    uint64_t start;
    uint32_t count;
} Stake;

/* A block this call wrote, and the digest of what it holds (format v2). */
typedef struct {
    uint64_t block;
    uint64_t digest;
} PlotDigest;

typedef struct {
    uint64_t      fid;
    uint32_t      mount_epoch;    /* the mounting the Plot's block numbers are of */
    TagFSMetadata meta;           /* the record as loaded: name, tags, flags, times */
    uint64_t      shape;          /* the Ink that gave the loaded record its size and extents */
    uint64_t      data_need;      /* the flush need the blocks this call wrote hold, 0 for none */
    uint64_t      data_us;        /* the medium's time on this call's data, measured, since its last Ink */
    uint64_t      size_before;    /* the size the call found; what existed is below it */
    uint64_t      size;           /* the size the call leaves */
    Strip        *runs;           /* the working runs, in file order */
    uint32_t      run_count;
    uint32_t      run_room;
    uint64_t      blocks;         /* how many blocks the runs name */
    uint64_t      blocks_before;  /* ... and how many the loaded record named */
    uint64_t      runs_before;    /* ... in how many extents of format v1 (PackV1Extents) */
    Stake        *stakes;
    uint32_t      stake_count;
    uint32_t      stake_room;
    Strip        *freed;          /* ground the record stops naming, to Fallow once it is inked */
    uint32_t      freed_count;
    uint32_t      freed_room;
    FileHelm     *helm;           /* the Helm its holder loaded it under; NULL for none */
    uint32_t      claimed;        /* the extents it claimed room for in the record (RT-19) */
    InkWarrant    warrant;        /* the call's, asked by the guard at the load and at the Ink */
    /* The volume's book is format v2: chosen once, by PlotLoad, and every
     * branch between the two formats reads it here. */
    bool          book;
    /* Format v2 alone (plot_window.c, PlotWindow): the record the Plot was
     * loaded from, held — its runs are found and walked where the record holds
     * them, never copied — and `runs` only the window of them a change works
     * on, whole Strips from block `window_at`; on format v1 the window is the
     * file, window_at 0 and `rec` NULL. A book's Plot whose file was given up
     * holds none, and every change of it is refused ERR_FILE_NOT_FOUND. */
    FileRecord   *rec;
    uint64_t      window_at;      /* the file's block runs[0] begins at */
    uint64_t      window_was;     /* the blocks of `rec` the window took in: what its splice takes out */
    bool          windowed;       /* the window is open */
    Strip        *fresh;          /* ground taken for blocks written over, from file block fresh_at on */
    uint32_t      fresh_count;
    uint32_t      fresh_room;
    uint64_t      fresh_at;
    uint64_t      fresh_blocks;
    PlotDigest   *digests;        /* every block this call wrote, with its digest */
    uint32_t      digest_count;
    uint32_t      digest_room;
    uint64_t      ribbon_held;    /* the ribbon's room held for its record from before its data to its
                                   * Ink (RibbonReserve), of the mounting `mount_epoch` names */
    /* A book's write that rides a Slip instead of a block (plot_slip.c):
     * its own TailSlip — the Plot's alone until its Ink folds it into the
     * tail the record keeps (TailSlipFold) — and the span of the last block
     * its bytes take; NULL for a write that lays blocks. */
    TailSlip     *slip;
    uint32_t      slip_off;
    uint32_t      slip_len;
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    bool          engross;        /* an Engross's: its Ink says so (EngrossInked, settle_cut.h) */
#endif
} Plot;

/* The file's record, out of the map of live files afresh — never a handle's
 * copy of it — for a call carrying `warrant`. OK, or ERR_FILE_NOT_FOUND,
 * ERR_METADATA_CORRUPTED (the record names fewer blocks than its size needs),
 * the refusal of the one guard of a change (InkMayChange: ERR_PERMISSION_DENIED
 * for a file the system keeps and a call with no warrant, and for the kernel
 * the loader reads by its blocks; ERR_DEVICE_NOT_READY once the medium left or
 * the halt shut the volume, ERR_TAGFS_READ_ONLY) — asked here, before a byte of
 * the call moves — or ERR_NO_MEMORY. A holder of the file's Helm sets `helm`
 * after the load, so the record's room is claimed for it (PlotReserve). */
error_t  PlotLoad(Plot *p, uint64_t fid, InkWarrant warrant);

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
bool     PlotFind(const Strip *runs, uint32_t count, uint64_t k,
                  uint64_t *out_block, uint32_t *out_left);

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

/*
 * A stretch: the part of a write that lies in one run on the medium and goes
 * out in one command (step 1.11). Only its first and last block can be partly
 * written, so the one read-modify-write rule is theirs alone; every block
 * between them the write covers whole.
 */
typedef struct {
    uint64_t block;    /* its first block on the medium */
    uint32_t count;    /* its blocks, in one run */
    uint32_t head;     /* where the write begins inside the first block */
    uint64_t bytes;    /* the write's bytes it carries */
    PlotFill first;    /* what the first block does with the rest of it */
    PlotFill last;     /* ... and the last — the first's own when it is one block */
    uint64_t from_first;  /* the block the rest of the first is read from: `block` itself on format
                           * v1, the file's old block on fresh ground (format v2, §3.1) */
    uint64_t from_last;   /* ... and of the last */
    uint64_t k;           /* the file's block its first block is: the Slips over the last lie under the
                           * write that reads it (format v2, tail_slip.h) */
} PlotStretch;

/* The stretch a write reaches at file byte `pos` with `left` of its bytes
 * still to go, at most `most` blocks: false when no run of the Plot holds the
 * block `pos` is in. */
bool     PlotStretchAt(const Plot *p, uint64_t pos, uint64_t left, uint32_t most,
                       PlotStretch *out);

/* Of a stretch, the write's bytes its first `blocks` blocks carry. */
uint64_t PlotStretchBytes(const PlotStretch *s, uint32_t blocks);

/* The run [start, start + count) after the Plot's last one: merged into it
 * when it begins where that one ends and is no longer than a Strip counts.
 * OK or ERR_NO_MEMORY, the runs unchanged. */
error_t  PlotAppendRun(Plot *p, uint64_t start, uint32_t count);

/* Whether a record fits format v1, its Strips counted as the extents PackV1
 * cuts them into: its length in *out_bytes when it does. */
bool     PlotFits(const TagFSMetadata *record, uint32_t *out_bytes);

/* The same, the record's runs given as the count of extents PackV1 would cut
 * them into (`record`'s own runs not asked): for a record whose runs memory
 * holds in a Tract — counted where they lie (PackV1RecordExtents) and judged
 * before they are laid flat for the packer. */
bool     PlotFitsExtents(const TagFSMetadata *record, uint64_t extents, uint32_t *out_bytes);

/* How many extents of format v1 a record has room for beside its tags and its
 * name: 0 when they leave it none. */
uint32_t PlotRunRoom(const TagFSMetadata *record);

/* How many extents format v1 cuts `count` Strips into: one for each 0xFFFF
 * blocks of a Strip, and one for what is left of it. */
uint64_t PackV1Extents(const Strip *strips, uint32_t count);

/* The same count of a record's Strips, walked where the record holds them. */
uint64_t PackV1RecordExtents(const FileRecord *rec);

/* Ground under every block of [0, end) bytes: what the file lacks is taken
 * through its Allotment near its last block, each run a Stake. Then the
 * record must fit: an extent list too long is shortened by PlotGather, and
 * ERR_TAGFS_RECORD_FULL — said, with its numbers — when it cannot be, or the
 * gather's own refusal as it gave it (ERR_WOULD_BLOCK while its run waits for
 * a cycle); and the room its extents need is claimed (InkRoomClaim). Nothing
 * is written but what a gather moves. */
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

/* Whether the volume could ever give the blocks [0, end) bytes need beyond
 * those the Plot names: every block free, every one resting in Fallow and every
 * one the pool's slack would give back counted as the Plot's to have. OK, or
 * ERR_TAGFS_NO_SPACE — said, with the numbers — when even all of them would not
 * cover it. Asked before a growth takes its first block, so one past what the
 * volume has takes none (Z07-5). Nothing is taken. */
error_t  PlotRoomFor(const Plot *p, uint64_t end);

/* The file grown to `size` with zeros: the old last block's bytes past the old
 * size zeroed (read first — a refused read refuses the span), and every block
 * after it taken and written as zeros. The Plot's size is `size` after OK. */
error_t  PlotExtendZero(Plot *p, uint64_t size);

/*
 * PlotGather — extents made fewer by moving the fewest blocks.
 *
 * The consecutive runs whose blocks, laid in one run, shed at least `shed`
 * extents from the record (PackV1Extents), the window with the fewest blocks
 * of all such: one run of exactly that many is taken near `hint`, the blocks
 * are copied into it and flushed, and the Plot names the run instead. The
 * ground left comes to rest once a record that no longer names it is inked.
 * `shed` 0 asks for every run of the file in one (defrag).
 *
 * Only ground the loaded record named is moved, never a Stake — and never a
 * block of a file the system keeps (B10): one wearing system, boot or god, or
 * holding the kernel the Deed points the loader at, is refused
 * ERR_PERMISSION_DENIED, said, whatever warrant the call carries — a change a
 * warrant lets into such a file lays its bytes where the file has them, and
 * moves none. Refused — said, with the numbers — when no window sheds enough
 * or no run that long is free.
 *
 * The same gather in windows, for a holder that gives its core back between
 * two (a defrag, R24): PlotGatherBegin chooses the extents and takes the run —
 * nothing is copied yet — and each PlotGatherStep copies the next window of
 * them, as many blocks as the seat takes in one command. The step that copies
 * the last makes the copy durable, asks B10 again of the record as it is then,
 * and makes the Plot name the run: *out_laid, and the caller inks. Nothing is
 * named before that, so a gather let go of part way gives back its run.
 * PlotGather is all of it in one call.
 */
typedef struct {
    uint32_t first;     /* the runs gathered, in the file's order ... */
    uint32_t last;
    uint64_t blocks;    /* ... the blocks they hold, which the run taken for them holds */
    uint64_t to;        /* where that run begins */
    uint64_t copied;    /* how many of them are copied into it */
    uint32_t extent;    /* where the copy goes on: the run, */
    uint32_t within;    /* ... and its block */
    bool     laid;      /* the Plot names the run */
} PlotGathering;

error_t  PlotGather(Plot *p, uint64_t shed, uint64_t hint);
error_t  PlotGatherBegin(Plot *p, uint64_t shed, uint64_t hint, PlotGathering *g);
error_t  PlotGatherStep(Plot *p, PlotGathering *g, bool *out_laid);

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

/* The source written at `at` synchronously, a stretch a command, each block
 * by the one read-modify-write rule. It stops at the first refusal in the
 * file's order — a read, the source's pages, a write — and *out_done is what
 * landed before it: the blocks of a stretch before a refused one are laid. */
error_t  PlotWriteBlocks(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done);

/*
 * PlotConveyance — what the Plot did to the runs it was loaded with, as the
 * splices of the record `cur` it loaded them from (file_record.h, BookSplice):
 * a new array the caller frees, *count splices — none when the runs are as
 * they were — each putting in Strips of the Plot's own runs, so the Plot is
 * not changed while they are used. Applied to `cur` (FileRecordSplice) they
 * make a record whose Strips are the Plot's, Strip for Strip, and give the
 * ground it let go. Asked by the Ink once it holds the record the Plot was
 * loaded from — the same shape (TagFSInkPlot). OK or ERR_NO_MEMORY.
 */
error_t  PlotConveyance(const Plot *p, const FileRecord *cur, BookSplice **out, uint64_t *count);

/* A book's Plot (plot_window.c): its window made the whole file — for a
 * change that moves every run (a defrag) — OK, or ERR_NO_MEMORY; nothing on
 * format v1, whose Plot holds the file. And `count` blocks this call wrote
 * from `block` on, `data` what they hold: each one's digest kept, for the
 * Gloss and the stitch that names it (§2.8, §8.1) — nothing on format v1,
 * whose writes record their digests as they land. OK or ERR_NO_MEMORY. */
error_t  PlotWindowWhole(Plot *p);
error_t  PlotWindowLanded(Plot *p, uint64_t block, const uint8_t *data, uint32_t count);

/* Engross (plot_slip.c, spec_v2 §16.5 п.14): a book's file whose last block
 * has Slips over it gets that block laid again on fresh ground — its base,
 * the Slips over it, zeros past the size — and the Plot names it, *laid; the
 * caller inks it, and the record lets the Slips go with the block they lay
 * on. OK and nothing done for a file with no Slip, or on format v1; or the
 * refusal of the ground, the read, the write or the ribbon's room. */
error_t  PlotEngross(Plot *p, bool *laid);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's measure that a write copies only the Strips around its change
 * (§2.9, H-5): from PlotWindowMeasure(fid) on, the most Strips one window of
 * the file `fid` copied out of its record — 0 for no file. */
void     PlotWindowMeasure(uint64_t fid);
uint64_t PlotWindowMeasured(void);
#endif

#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
/* After every Ink of format v1 (plot_conveyance.c): whether memory's record
 * `rec` is the record format v1 packed (`packed`, `bytes` long) — its header
 * field by field, its blocks where its Strips lay them. True when it is; said
 * — and in a STRICT kernel stopped at — when not. */
bool     PackV1Agrees(const FileRecord *rec, const uint8_t *packed, uint32_t bytes);

/* After a Plot's Ink, the record `rec` that `sp` (nsp splices, PlotConveyance)
 * built from `cur` held to all three copies of what the Ink lays: it is the
 * record v1 packed (PackV1Agrees), its Strips are the Plot's runs Strip for
 * Strip, and the ground the splices free is the ground the Plot let go. */
bool     PlotConveyanceAgrees(const Plot *p, const FileRecord *cur, const BookSplice *sp,
                              uint64_t nsp, const FileRecord *rec, const uint8_t *packed,
                              uint32_t bytes);
#endif

/* A record packed for the medium into `out` — room for the length PlotFits
 * gives — each Strip cut into extents of at most PLOT_RUN_MOST blocks: OK and
 * its length in *out_bytes. `run_blocks` is the volume's data run, which every
 * run must lie inside (0 before a Deed has said it: none judged). Refused,
 * said, before a byte of `out` is written:
 * ERR_TAGFS_FID_EXHAUSTED for a file id past the u32 a record carries,
 * ERR_TAGFS_RECORD_FULL for a run past what one carries — a block past a u32,
 * a run that owns no ground (STRIP_HOLE) — or a record wider than one holds
 * (its tags, its extents after the cut, its name), and ERR_TAGFS_METADATA_ERROR
 * for a run of no blocks or one outside the data run. */
error_t  PackV1(const TagFSMetadata *meta, uint64_t run_blocks, uint8_t *out, uint32_t *out_bytes);

/* A packed record given the file id `fid`, and summed again: OK, or
 * ERR_TAGFS_FID_EXHAUSTED — said, the record untouched — for an id past the
 * u32 a record carries. */
error_t  PackV1Refile(uint8_t *rec, uint64_t fid);

/* A packed record read back, whose room in its block is `room` bytes, its
 * extents as Strips — neighbours on the medium joined into one: OK,
 * ERR_METADATA_CORRUPTED when it does not check out, or ERR_NO_MEMORY. One
 * whose extents name a block outside the data run of `run_blocks` blocks (as
 * PackV1 judges it), or none at all, comes out naming no blocks, and
 * *out_dropped says so (NULL: not asked). */
error_t  UnpackV1(const uint8_t *rec, uint32_t room, uint64_t run_blocks, TagFSMetadata *out,
                  bool *out_dropped);

/* The file id of a packed record that checks out as UnpackV1 takes it, or 0
 * when it does not — reading only, taking no memory. */
uint64_t UnpackV1Fid(const uint8_t *rec, uint32_t room);

#endif /* TAGFS_PLOT_H */
