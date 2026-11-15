#include "stand_stitch.h"
#include "stand_fresh.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../file_map/file_map.h"
#include "../file_map/tract.h"
#include "../block_map/block_map_derived.h"
#include "../settle/binder.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_fresh_ground suite: where fresh ground is taken on a volume, what
// a write of it costs memory, and the reserve it leaves
//
// A file written at its end keeps its ground in one run — the block that
// continues its last run is taken while it is free (Allotment 1) — and blocks
// written over in one call land in one new run (Allotment 2). A block written
// over in a file of thousands of runs copies one Parcel of them and the
// record's head, never the file: the Plot holds a window around the write, not
// the runs. Data never takes the ground the book keeps for its ribbon and its
// Tally (Endpaper): filled to its refusal and emptied, the volume gives the
// data's ground back — weighed where the measure stands, the wall's wait on a
// Tally held down (BinderStandNoRoomWait): a turn of the ring at the wall gives
// a slot's ground back, as much as a ribbon that went through Quires since the
// slots' Tallies left them, and the Tallies it writes are no ribbon's to put
// back. A write longer than any one stitch could name lands in parts, and
// reads back whole.
// ============================================================================

/* The volume's free blocks the data may never take: the Tally's two copies and
 * a Quire of ribbon. */
static uint64_t StandFreshKept(const StandStitch* s)
{
    return EndpaperKeeps(s->at.quire_blocks) + s->at.quire_blocks;
}

// ----------------------------------------------------------------------------
// A file written at its end keeps one run; blocks written over in one call
// land in one (Allotment 1 and 2)
// ----------------------------------------------------------------------------

#define STAND_FRESH_APPENDS  1000u

/* Whether the take that laid file block `k` off the run before it passed over
 * the block that would have continued that run while it was free — free on
 * the map and in no other file's window, as the take asks (allotment.c,
 * AllotmentBookPick (1)). Judged as the take left the ground, before a cycle
 * gives any back: ground a Tally's DONE or a freer rested in Fallow was no
 * free block at the take, and is one by the time the log is done. *named
 * false when the record does not name blocks k - 1 and k as ground — an
 * append's block missing from the log it was written into. */
static bool StandFreshPassedAt(const FileRecord* rec, uint64_t fid, uint64_t k, bool* named)
{
    FileSpot before = { 0, 0, 0 }, at = { 0, 0, 0 };
    bool     passed = false;
    *named = FileRecordFind(rec, k, &at) && at.kind == STRIP_DATA &&
             (k == 0 || (FileRecordFind(rec, k - 1u, &before) && before.kind == STRIP_DATA));
    if (!*named || k == 0 || at.block == before.block + 1u)
        return false;
    TagFSLockTake(&tagfs_get_state()->lock);
    passed = BlockMapDerivedFreeFrom(before.block + 1u, 1u, fid) > 0;
    TagFSLockDrop(&tagfs_get_state()->lock);
    return passed;
}

/* `count` appends of `bytes` each, a cycle settled every hundred: true, or
 * false at the first append refused, a block it filled its record does not
 * name, or a cycle refused — said. Each block an append fills is judged where
 * it lands at once (StandFreshPassedAt): *passed counts those laid past a
 * free block. */
static bool StandFreshAppends(StandFreshModel* m, uint64_t fid, uint32_t count, uint32_t bytes, uint64_t* passed)
{
    for (uint32_t i = 0; i < count; i++) {
        uint64_t k = m->size / TAGFS_BLOCK_SIZE;
        if (StandFreshWrite(m, fid, m->size, bytes, 100u + i) != (int)bytes)
            return false;
        FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
        bool        named = rec != NULL;
        while (named && k < m->size / TAGFS_BLOCK_SIZE) {
            *passed += StandFreshPassedAt(rec, fid, k, &named);
            k += named ? 1u : 0u;      /* k stays on a block the record does not name, to be said */
        }
        FileRecordRelease(rec);
        if (!named) {
            kprintf("[TESTS] stand_fresh_ground: file %llu's record does not name block %llu of the %llu its "
                    "appends wrote as ground\n", (unsigned long long)fid, (unsigned long long)k,
                    (unsigned long long)(m->size / TAGFS_BLOCK_SIZE));
            return false;
        }
        if (i % 100u == 99u && TagFSSettle() != OK)
            return false;
    }
    return true;
}

/* The file's runs of ground, and in *passed — when asked — the breaks between
 * two where the block that would have continued the run before is free now
 * and in no other file's window (Allotment 1): a break forced is one only
 * ground another owner holds made, the book's own Quires among it, and no
 * bound of the stand's counts the runs. `tail` lets the last run stand alone:
 * an unfilled last block is put aside, so the block it would continue on stays
 * free for the version that fills it. A window a take begins anew is sized to
 * the file (Allotment 2), so a hole shorter than it past that block is passed
 * over by the rule, not against it. */
static uint64_t StandFreshRuns(uint64_t fid, bool tail, uint64_t* passed)
{
    FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    runs = 0, end = 0, free = 0;
    StripWalk   walk;
    Strip       s;
    if (!rec)
        return 0;
    StripWalkBegin(rec, &walk);
    for (uint64_t k = 0; StripWalkNextGround(&walk, &s); k += s.count) {
        if (passed && runs && s.start != end && !(tail && k + s.count == rec->blocks)) {
            TagFSLockTake(&tagfs_get_state()->lock);
            free += BlockMapDerivedFreeFrom(end, 1u, fid) > 0;
            TagFSLockDrop(&tagfs_get_state()->lock);
        }
        runs += !runs || s.start != end;
        end   = s.start + s.count;
    }
    if (passed)
        kprintf("[TESTS] stand_fresh_ground: file %llu, %llu block(s), lies in %llu run(s) of ground, %llu of them "
                "begun where the block that would have gone on the run before is free\n", (unsigned long long)fid,
                (unsigned long long)rec->blocks, (unsigned long long)runs, (unsigned long long)free);
    else
        kprintf("[TESTS] stand_fresh_ground: file %llu, %llu block(s), lies in %llu run(s) of ground\n",
                (unsigned long long)fid, (unsigned long long)rec->blocks, (unsigned long long)runs);
    FileRecordRelease(rec);
    if (passed)
        *passed = free;
    return runs;
}

/* The runs as the ground stands now — judged before a cycle may give any back
 * since the writes took it: 0 when a break was not forced. */
uint64_t StandFreshForcedRuns(uint64_t fid, bool tail)
{
    uint64_t passed = 0, runs = StandFreshRuns(fid, tail, &passed);
    return passed ? 0 : runs;
}

/* Whether the log's record holds no more Strips than its appends make: one a
 * cycle — the appends a cycle takes go on the run its first one began, and a
 * run a settled stitch named is never grown, so the next cycle's begin their
 * own — one more a run of ground the log lies in, and its unfilled last block
 * apart (`tail`). */
static bool StandFreshLogStrips(uint64_t fid, uint64_t runs, bool tail)
{
    FileRecord* rec    = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    strips = rec ? rec->strip_count : ~0ull;
    FileRecordRelease(rec);
    TestEvidence("Strips the record of a log of a thousand appends holds", strips);
    return strips <= STAND_FRESH_APPENDS / 100u + runs + (tail ? 1u : 0u);
}

static TestResult TestStandFreshAppendsKeepOneRun(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, runs = 0, passed = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, (uint64_t)STAND_FRESH_APPENDS * TAGFS_BLOCK_SIZE), "room for the log's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-log.txt", NULL, 0, &fid), 0, "a log is made on the book");
    TEST_ASSERT(StandFreshAppends(&m, fid, STAND_FRESH_APPENDS, TAGFS_BLOCK_SIZE, &passed),
                "a thousand blocks are appended, each named by the log's record as it lands");
    runs = StandFreshRuns(fid, false, NULL);
    TestEvidence("runs of ground a log of a thousand appended blocks lies in", runs);
    TestEvidence("blocks laid off the run while the block that went on it was free", passed);
    TEST_ASSERT(runs >= 1 && passed == 0,
                "and each block continued the run before it wherever that block was free as its take found the "
                "ground — a run broken only by ground another owner holds");
    TEST_ASSERT(StandFreshLogStrips(fid, runs, false), "and its record a Strip a cycle on each run, not one an append");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, it reads them all");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-lines.txt", NULL, 0, &fid), 0, "a second log is made");
    m.size = 0;
    TEST_ASSERT(StandFreshAppends(&m, fid, STAND_FRESH_APPENDS, 333u, &passed),
                "a thousand lines of 333 bytes are appended, each block they fill named as it lands");
    runs = StandFreshRuns(fid, true, NULL);
    TestEvidence("runs of ground a log of a thousand appended lines lies in", runs);
    TestEvidence("blocks laid off the run while the block that went on it was free", passed);
    TEST_ASSERT(runs >= 1 && passed == 0,
                "and the version that filled each block went on the block that continued the run, its unfilled "
                "last block alone apart");
    TEST_ASSERT(StandFreshLogStrips(fid, runs, true), "and its record a Strip a cycle on each run, not one an append");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, it reads them all");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

#define STAND_FRESH_SPAN_BLOCKS  64u

static TestResult TestStandFreshOverwriteOneRun(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, first = 0, block = 0, old = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_FRESH_SPAN_BLOCKS * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-over.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, STAND_FRESH_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, 61),
                   (int)(STAND_FRESH_SPAN_BLOCKS * TAGFS_BLOCK_SIZE), "64 blocks are written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid) && StandBlockOf(fid, 8, &old), "and settled");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 8u * TAGFS_BLOCK_SIZE, 32u * TAGFS_BLOCK_SIZE, 62),
                   (int)(32u * TAGFS_BLOCK_SIZE), "blocks 8 to 39 are written over in one call");
    TEST_ASSERT(StandBlockOf(fid, 8, &first) && first != old, "on fresh ground");
    for (uint64_t k = 9; k < 40; k++)
        TEST_ASSERT(StandBlockOf(fid, k, &block) && block == first + (k - 8u),
                    "every one of them in the one new run the call took");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, the file reads the new bytes");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A block written over in a file of thousands of runs copies one Parcel and
// the record's head (§2.9, §3.7, H-5)
// ----------------------------------------------------------------------------

#define STAND_FRESH_PARCEL_BLOCKS  8192u   /* 32 MiB */
#define STAND_FRESH_PARCEL_EVERY   16u
#define STAND_FRESH_PARCEL_WRITES  1000u

/* The Parcels under `t`, into out[] — at most `room` — in the file's order:
 * how many, or room + 1 past it. */
static uint32_t StandFreshParcels(const Tract* t, const Parcel** out, uint32_t room)
{
    if (!t)
        return 0;
    if (t->level == 0) {
        for (uint32_t i = 0; i < t->count; i++)
            if (i < room)
                out[i] = t->kid[i].node;
        return t->count <= room ? t->count : room + 1u;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->count && n <= room; i++)
        n += StandFreshParcels(t->kid[i].node, out + n, room - n);
    return n;
}

#define STAND_FRESH_PARCEL_ROOM 512u
#define STAND_FRESH_NODE_ROOM   64u

/* The Tract nodes under `t`, `t` among them, into out[] — at most `room` — :
 * how many, or room + 1 past it. */
static uint32_t StandFreshNodes(const Tract* t, const Tract** out, uint32_t room)
{
    if (!t)
        return 0;
    if (room == 0)
        return 1;
    out[0]     = t;
    uint32_t n = 1;
    for (uint32_t i = 0; t->level > 0 && i < t->count && n <= room; i++)
        n += StandFreshNodes(t->kid[i].node, out + n, room - n);
    return n;
}

/* What a change built that the record before it did not have: the Strips in
 * Parcels of its own and how many such Parcels, and the heap it took — its
 * header, those Parcels and every Tract node not shared — in how many
 * allocations. */
typedef struct {
    uint64_t strips;
    uint64_t bytes;
    uint32_t parcels;
    uint32_t allocs;
} StandFreshBuilt;

/* What `now` built that `was` does not share: false past the room. */
static bool StandFreshCopied(const FileRecord* was, const FileRecord* now, StandFreshBuilt* b)
{
    static const Parcel* a[STAND_FRESH_PARCEL_ROOM];
    static const Parcel* c[STAND_FRESH_PARCEL_ROOM];
    static const Tract*  x[STAND_FRESH_NODE_ROOM];
    static const Tract*  y[STAND_FRESH_NODE_ROOM];
    uint32_t na = StandFreshParcels(now->tract, a, STAND_FRESH_PARCEL_ROOM);
    uint32_t nc = StandFreshParcels(was->tract, c, STAND_FRESH_PARCEL_ROOM);
    uint32_t nx = StandFreshNodes(now->tract, x, STAND_FRESH_NODE_ROOM);
    uint32_t ny = StandFreshNodes(was->tract, y, STAND_FRESH_NODE_ROOM);
    if (na > STAND_FRESH_PARCEL_ROOM || nc > STAND_FRESH_PARCEL_ROOM || nx > STAND_FRESH_NODE_ROOM ||
        ny > STAND_FRESH_NODE_ROOM)
        return false;
    memset(b, 0, sizeof(*b));
    b->bytes  = now->bytes - (now->tract ? now->tract->bytes : 0);
    b->allocs = 1;
    for (uint32_t i = 0; i < na; i++) {
        bool shared = false;
        for (uint32_t j = 0; j < nc && !shared; j++)
            shared = a[i] == c[j];
        if (!shared) {
            b->strips += a[i]->count;
            b->bytes  += ParcelBytes(a[i]->count);
            b->parcels++;
            b->allocs++;
        }
    }
    for (uint32_t i = 0; i < nx; i++) {
        bool shared = false;
        for (uint32_t j = 0; j < ny && !shared; j++)
            shared = x[i] == y[j];
        if (!shared) {
            b->bytes += sizeof(Tract) + (uint64_t)x[i]->count * sizeof(TractKid);
            b->allocs++;
        }
    }
    return true;
}

/* The most a write inside one block may build: the Strips of one Parcel and
 * the two a write in the middle of a run adds — in two Parcels where that one
 * is split — and the record's head: its header, and a node of the Tract a
 * level on the way down to them. */
static uint64_t StandFreshBuiltMost(const FileRecord* rec)
{
    uint32_t height = rec->tract ? rec->tract->level + 1u : 0;
    return ParcelBytes(PARCEL_STRIPS_MOST + 2u) + ParcelBytes(0) + (rec->bytes - (rec->tract ? rec->tract->bytes : 0)) +
           (uint64_t)height * (sizeof(Tract) + TRACT_KIDS_MOST * sizeof(TractKid));
}

/* The file of STAND_FRESH_PARCEL_BLOCKS blocks, every STAND_FRESH_PARCEL_EVERY-th
 * written over so its runs are cut into more than three Parcels. */
static bool StandFreshManyRuns(StandFreshModel* m, uint64_t fid)
{
    if (StandFreshWrite(m, fid, 0, STAND_FRESH_PARCEL_BLOCKS * TAGFS_BLOCK_SIZE, 71) !=
        (int)(STAND_FRESH_PARCEL_BLOCKS * TAGFS_BLOCK_SIZE))
        return false;
    for (uint32_t k = 0; k < STAND_FRESH_PARCEL_BLOCKS; k += STAND_FRESH_PARCEL_EVERY)
        if (StandFreshWrite(m, fid, (uint64_t)k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 72) != TAGFS_BLOCK_SIZE)
            return false;
    return TagFSSettle() == OK;
}

static TestResult TestStandFreshParcelCopied(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    StandFreshBuilt most = { 0 };
    TagFSState*     fs  = tagfs_get_state();
    uint64_t        fid = 0, window = 0, bound = 0, seed = 0x9E3779B97F4A7C15ull;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, (uint64_t)STAND_FRESH_PARCEL_BLOCKS * TAGFS_BLOCK_SIZE), "room for 32 MiB");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-parcels.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT(StandFreshManyRuns(&m, fid), "32 MiB written, and one block of every sixteen written over");
    FileRecord* rec = FileMapRecordHold(fs->files, fid, NULL);
    uint64_t    parcels = rec ? rec->strip_count / PARCEL_STRIPS_MOST : 0;
    bound = rec ? StandFreshBuiltMost(rec) : 0;
    FileRecordRelease(rec);
    TEST_ASSERT(parcels >= 3, "its runs are cut into three Parcels and more");
    PlotWindowMeasure(fid);
    for (uint32_t i = 0; i < STAND_FRESH_PARCEL_WRITES; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t        k     = (seed >> 33) % STAND_FRESH_PARCEL_BLOCKS;
        FileRecord*     was   = FileMapRecordHold(fs->files, fid, NULL);
        StandFreshBuilt built = { 0 };
        int             wrote = StandFreshWrite(&m, fid, k * TAGFS_BLOCK_SIZE + 100u, 200u, 73u + i);
        FileRecord*     now   = FileMapRecordHold(fs->files, fid, NULL);
        bool            held  = was && now && StandFreshCopied(was, now, &built);
        FileRecordRelease(was);
        FileRecordRelease(now);
        TEST_ASSERT(wrote == 200 && held, "200 bytes inside a block chosen at random are written");
        most.strips  = built.strips > most.strips ? built.strips : most.strips;
        most.bytes   = built.bytes > most.bytes ? built.bytes : most.bytes;
        most.parcels = built.parcels > most.parcels ? built.parcels : most.parcels;
        most.allocs  = built.allocs > most.allocs ? built.allocs : most.allocs;
        if (i % 100u == 99u)
            TEST_ASSERT_OK(TagFSSettle(), "a cycle settles every hundred writes");
    }
    window = PlotWindowMeasured();
    PlotWindowMeasure(0);
    kprintf("[TESTS] stand_fresh_ground: %u writes inside a block of a file of %llu Parcels — the most one built of "
            "its own %llu Strips in %u Parcel(s), %llu bytes of the heap in %u allocation(s) (bound %llu); the most "
            "its window copied out of the record %llu\n", STAND_FRESH_PARCEL_WRITES, (unsigned long long)parcels,
            (unsigned long long)most.strips, most.parcels, (unsigned long long)most.bytes, most.allocs,
            (unsigned long long)bound, (unsigned long long)window);
    TEST_ASSERT(most.strips <= PARCEL_STRIPS_MOST + 2u && most.parcels <= 2u,
                "no write built more Strips than a Parcel holds and the two a write inside a run adds — two Parcels "
                "at the most, where that one is split");
    TEST_ASSERT(most.bytes <= bound,
                "and no write took more of the heap than those Strips and the record's head — its header and a "
                "Tract node a level");
    TEST_ASSERT(window == 1u, "and a write inside one block copied into its window the one Strip that holds it");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, the file reads every byte written");
out:
    PlotWindowMeasure(0);
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// Data never takes the book's reserve; filled and emptied, the volume gives
// the data's ground back (§3.6, §16.2 п.6)
// ----------------------------------------------------------------------------

#define STAND_FRESH_FILL_ROOM   96u
#define STAND_FRESH_FILL_FILE   8u
#define STAND_FRESH_FILL_MOST   64u

/* STAND_FRESH_FILL_FILE blocks written into file `fid`: the write's answer. */
static error_t StandFreshFill(uint64_t fid)
{
    TagFSFileHandle* h       = tagfs_open(fid, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    uint64_t         written = 0;
    PlotSource       src     = { .kernel = g_test_buffer, .bytes = sizeof(g_test_buffer) };
    error_t          rc      = h ? OK : ERR_FILE_NOT_FOUND;
    for (uint32_t b = 0; b < STAND_FRESH_FILL_FILE && rc == OK; b += sizeof(g_test_buffer) / TAGFS_BLOCK_SIZE)
        rc = tagfs_write(h, &src, &written);
    if (h)
        tagfs_close(h);
    return rc;
}

static TestResult TestStandFreshFillAndEmpty(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagFSState* fs     = tagfs_get_state();
    uint64_t    fids[STAND_FRESH_FILL_MOST], kept, free, quires;
    uint32_t    made   = 0;
    error_t     rc     = OK;
    bool        shrunk = false;
    const char* aside  = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    kept = StandFreshKept(&s);
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(BlockMapDerivedStandShrink(kept + STAND_FRESH_FILL_ROOM), "the volume is cut to 96 blocks past its reserve");
    shrunk = true;
    free   = fs->free_blocks;
    quires = __atomic_load_n(&RibbonCursorNow()->quires, __ATOMIC_ACQUIRE);
    TestFillAt(g_test_buffer, sizeof(g_test_buffer), 81, 0);
    while (made < STAND_FRESH_FILL_MOST && rc == OK) {
        TEST_ASSERT_EQ(tagfs_create_file("fresh-fill.bin", NULL, 0, &fids[made]), 0, "a file is made");
        rc = StandFreshFill(fids[made++]);
        uint64_t went = __atomic_load_n(&RibbonCursorNow()->quires, __ATOMIC_ACQUIRE) - quires;
        TEST_ASSERT(fs->free_blocks + went * s.at.quire_blocks >= kept,
                    "and the data leaves the book's reserve free — but the Quires the ribbon went on into, "
                    "which are what it is kept for");
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_NO_SPACE, "filled, the volume refuses the next write: no space");
    for (uint32_t i = 0; i < made; i++)
        TEST_ASSERT_EQ(tagfs_delete_file(fids[i], INK_UNWARRANTED), 0, "every file is given up");
    TEST_ASSERT_OK(SettleForRoom(), "room is asked for");
    quires = __atomic_load_n(&RibbonCursorNow()->quires, __ATOMIC_ACQUIRE) - quires;
    TEST_ASSERT(fs->free_blocks + quires * s.at.quire_blocks >= free,
                "and the data's ground came back — all of it but the Quires the ribbon went on into");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write longer than a stitch could name lands in parts, and reads back whole
// ----------------------------------------------------------------------------

#define STAND_FRESH_BIG_BYTES  (16u * 1024u * 1024u)

static TestResult TestStandFreshBigWrite(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (!StandFreshModelMake(&m, STAND_FRESH_BIG_BYTES)) {
        STAND_STITCH_PUT_BACK(&s);
        return test_steps_aside("the heap holds no 16 MiB for the write's bytes");
    }
    TEST_ASSERT_EQ(tagfs_create_file("fresh-big.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, STAND_FRESH_BIG_BYTES, 91), (int)STAND_FRESH_BIG_BYTES,
                   "16 MiB are written in one call");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, every byte reads back");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_fresh_ground_tests[] = {
    { .name = "stand_fresh_ground_appends_keep_one_run", .func = TestStandFreshAppendsKeepOneRun,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_ground_overwrite_one_run", .func = TestStandFreshOverwriteOneRun,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_ground_parcel_copied", .func = TestStandFreshParcelCopied,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_ground_fill_and_empty", .func = TestStandFreshFillAndEmpty,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_ground_big_write", .func = TestStandFreshBigWrite,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_fresh_ground_suite = {
    g_stand_fresh_ground_tests, sizeof(g_stand_fresh_ground_tests) / sizeof(g_stand_fresh_ground_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
