#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/foliation.h"
#include "../settle/settle.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../file_map/file_map.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_tip_in suite: a Tally laid in among the nodes of the one before it
// (spec_v2 §4.1, §4.3 TALLY_NODES, §16.5 пп.16–17, §16.2 п.4, §10.4 step 2.4)
//
// A Tally writes the nodes whose keys changed since the Tally before it and
// the inner nodes above them, and names every other node as it lies, by the
// pointer it had: a file of three Parcels and more, one Parcel of it written
// over a thousand times, costs FILES the leaves that Parcel and the file's
// header lie in and the path above them — no other node of the tree is laid
// anew (C23); written a block apart, it comes back from the mount in the
// Parcels its Tally laid, one for one (§16.1 п.7). A copy of a node that would
// not read, a leaf of FILES at the mount or a leaf of DIGESTS at a read of a
// block it names, puts the node's keys among the changed ones: the next Tally
// lays it anew and every copy
// holds after it (C24). A volume
// filled and emptied by Tallies laid in so gives its ground back, every block
// the test took free again or held by the slots (§16.5 п.17: a Quire of the
// Tallies' held while one node of it lives). The tests read the Tally a mount
// stands on as the mount's walk reached it (TallyStandNodeWhere) and the
// reads' DIGESTS table, the runner holding the cadence (tests.c,
// SettleCadenceHold), so no Tally runs but the one a test asks for. Every
// test puts the Spine and the ribbon back as it found them.
// ============================================================================

#define STAND_TIP_IN_STRIDED  300u     /* blocks written one apart: a Strip each, a hole between — three Parcels */
#define STAND_TIP_IN_MANY     160u     /* files of a long name after it: FILES takes leaves past the file */
#define STAND_TIP_IN_WRITES   1000u    /* writes over the one Parcel */
#define STAND_TIP_IN_BATCH    100u     /* writes a stitch carries */

/* The Tally the mount stands on: its seq. */
static uint64_t StandTipInStood(void)
{
    return g_book.stood ? g_book.stood->head.tally_seq : 0;
}

/* Whether the keys [lo, hi) of a leaf's place meet [a, b). */
static bool StandTipInMeets(const TallyPlace* p, TallyKey a, TallyKey b)
{
    bool below_hi = !p->has_hi || a.k1 < p->hi.k1 || (a.k1 == p->hi.k1 && a.k2 < p->hi.k2);
    bool above_lo = !p->has_lo || p->lo.k1 < b.k1 || (p->lo.k1 == b.k1 && p->lo.k2 < b.k2);
    return below_hi && above_lo;
}

/* What the walk of FILES of the Tally stood on reached: every node, the ones
 * sealed at `seq`, the leaves of them whose keys meet [a, b), and the root's
 * level — the path from a leaf up. */
typedef struct {
    uint32_t nodes;
    uint32_t fresh;
    uint32_t meeting;
    uint64_t depth;
} StandTipInFiles;

static void StandTipInFilesOf(uint64_t seq, TallyKey a, TallyKey b, StandTipInFiles* out)
{
    TallyPlace p;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; TallyStandNodeWhere(TALLY_FILES, i, &p); i++) {
        if (i == 0)
            out->depth = p.level;
        out->nodes++;
        if (p.ptr.seq != seq)
            continue;
        out->fresh++;
        if (p.level == 0 && StandTipInMeets(&p, a, b))
            out->meeting++;
    }
}

/* Parcel `which` of the record of `fid`: the file block it begins at and the
 * blocks it covers. */
static bool StandTipInParcel(uint64_t fid, uint32_t which, uint64_t* from, uint64_t* blocks)
{
    FileRecord*  rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    StripWalk    w;
    const Strip* s = NULL;
    uint32_t     n = 0, at = 0;
    uint64_t     pos = 0;
    bool         found = false;
    if (!rec)
        return false;
    StripWalkBegin(rec, &w);
    while (!found && StripWalkParcel(&w, &s, &n)) {
        uint64_t covers = 0;
        for (uint32_t i = 0; i < n; i++)
            covers += s[i].count;
        if (at++ == which) {
            *from   = pos;
            *blocks = covers;
            found   = true;
        }
        pos += covers;
    }
    FileRecordRelease(rec);
    return found;
}

/* A file made as a program's call makes one: ERR_WOULD_BLOCK — the records
 * claimed since the last stitch fill the ribbon's room, and its next Quire is
 * on its way — waited out by the cycle that lays them and brings the room,
 * as boxlib waits it out; the answer after that cycle is the answer. */
static int StandTipInCreate(const char* name, uint64_t* fid)
{
    int rc = tagfs_create_file(name, NULL, 0, fid);
    if (rc == -(int)ERR_WOULD_BLOCK && TagFSSettle() == OK)
        rc = tagfs_create_file(name, NULL, 0, fid);
    return rc;
}

/* `n` files of a long name made from `from` on, settled. */
static bool StandTipInMake(uint32_t from, uint32_t n)
{
    char     name[80];
    uint64_t fid = 0;
    for (uint32_t i = 0; i < n; i++) {
        ksnprintf(name, sizeof(name), "tipin-%05u-of-a-name-long-enough-to-fill-leaves.txt", from + i);
        if (StandTipInCreate(name, &fid) != 0)
            return false;
    }
    return TagFSSettle() == OK;
}

// ----------------------------------------------------------------------------
// C23: a Parcel written over a thousand times costs FILES its leaves and the
// path above them
// ----------------------------------------------------------------------------

static TestResult TestStandTipInParcel(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    StandTipInFiles    whole, laid;
    uint64_t           fid = 0, from = 0, blocks = 0, seed = 0x5DEECE66Dull;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandFreshModelMake(&m, 2u * STAND_TIP_IN_STRIDED * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("tipin-parcels.bin", NULL, 0, &fid), 0, "a file is made first");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 2u * STAND_TIP_IN_STRIDED * TAGFS_BLOCK_SIZE), "spanned over a hole");
    for (uint64_t k = 0; k < STAND_TIP_IN_STRIDED; k++)
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 2u * k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 70u + k),
                       TAGFS_BLOCK_SIZE, "every other block written");
    TEST_ASSERT(StandTipInMake(0, STAND_TIP_IN_MANY), "and files of long names after it, settled");
    TEST_ASSERT(StandTipInParcel(fid, 2u, &from, &blocks), "the file has a third Parcel");
    TEST_ASSERT(StandTipInParcel(fid, 1u, &from, &blocks) && blocks >= 4u, "the middle Parcel, its blocks");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it all is written");
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounts on it, a stitch of it settled");
    TallyKey lo = { fid, 0 }, hi = { fid, 1u + from + blocks };
    StandTipInFilesOf(StandTipInStood(), lo, hi, &whole);
    TestEvidence("nodes of FILES the Tally stood on holds", whole.nodes);
    TEST_ASSERT(whole.depth >= 1 && whole.nodes > 2u + whole.depth, "FILES takes leaves past the Parcel's");
    for (uint32_t k = 0; k < STAND_TIP_IN_WRITES; k++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t at = from + ((seed >> 33) % (blocks / 2u)) * 2u;   /* a written block: the stride's */
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, at * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 900u + k),
                       TAGFS_BLOCK_SIZE, "a block of the middle Parcel written over");
        if (k % STAND_TIP_IN_BATCH == STAND_TIP_IN_BATCH - 1u)
            TEST_ASSERT_OK(TagFSSettle(), "the writes settle a batch at a time");
    }
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is written over them");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    StandTipInFilesOf(StandTipInStood(), lo, hi, &laid);
    TestEvidence("nodes of FILES the Tally laid anew", laid.fresh);
    TestEvidence("of them, leaves the file's header and the written Parcel lie in", laid.meeting);
    TestEvidence("levels above the leaves", laid.depth);
    TEST_ASSERT(laid.meeting >= 1, "the Parcel's leaf is laid anew");
    TEST_ASSERT(laid.fresh <= laid.meeting + laid.depth,
                "and nothing of FILES but the leaves the file's header and the Parcel lie in and the path above "
                "them (C23)");
    TEST_ASSERT(laid.nodes == whole.nodes || laid.nodes + 1u == whole.nodes || laid.nodes == whole.nodes + 1u,
                "the tree as many nodes as before, one either way");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and the file reads every byte written");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A file comes back from the mount in the Parcels its Tally laid (§16.1 п.7)
// ----------------------------------------------------------------------------

/* The block each Parcel of `fid`'s record begins at, in order: how many, at
 * most `most`. */
static uint32_t StandTipInCut(uint64_t fid, uint64_t* at, uint32_t most)
{
    uint32_t n = 0;
    uint64_t blocks = 0;
    while (n < most && StandTipInParcel(fid, n, &at[n], &blocks))
        n++;
    return n;
}

/*
 * Written a block apart, a file is cut where its splices cut it — each write
 * that outgrew a Parcel halving that Parcel alone (file_map/tract.c
 * SpliceGather), not the even cut of all its Strips — and a Tally lays memory's
 * Parcels one for one. The mount reads them as they lie: were it to cut the
 * file's Strips again, memory would hold a file its Tally does not, and DogEar
 * — the keys changed since that Tally — would not hold a key the new cut moved:
 * the next Tally laid in among this one's nodes keeps a leaf of the old cut
 * beside memory's new one, which the Binder's self-check refuses as a DEFECT.
 */
static TestResult TestStandTipInParcelsAsTheyLie(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, cut[16], back[16];
    uint64_t           even = (2u * STAND_TIP_IN_STRIDED + PARCEL_STRIPS_MOST - 1u) / PARCEL_STRIPS_MOST;
    uint32_t           n = 0, nback = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandFreshModelMake(&m, 2u * STAND_TIP_IN_STRIDED * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("tipin-cut.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 2u * STAND_TIP_IN_STRIDED * TAGFS_BLOCK_SIZE), "spanned over a hole");
    for (uint64_t k = 0; k < STAND_TIP_IN_STRIDED; k++)
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 2u * k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 70u + k),
                       TAGFS_BLOCK_SIZE, "every other block written: a Strip each, a hole between");
    n = StandTipInCut(fid, cut, ARRAY_SIZE(cut));
    TestEvidence("Parcels the splices cut the file into", n);
    TEST_ASSERT(n > even, "the splices cut it into more Parcels than an even cut of its Strips gives");
    TEST_ASSERT_OK(TagFSSettle(), "the writes settle");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    nback = StandTipInCut(fid, back, ARRAY_SIZE(back));
    TEST_ASSERT(nback == n && memcmp(cut, back, sizeof(uint64_t) * n) == 0,
                "the mount holds the file in the Parcels its Tally laid, one for one");
    uint64_t at = (cut[n / 2u] + 1u) & ~1ull;   /* a written block of a Parcel in the middle */
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, at * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 900u), TAGFS_BLOCK_SIZE,
                   "a block of it written over");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_OK(BinderRunWhole(), "the Tally after it is written: DogEar held every key the write changed");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and the file reads every byte written");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// C24: a copy of a leaf that would not read — the next Tally lays the leaf anew
// ----------------------------------------------------------------------------

/* Whether `a` and `b`, the two copies of a node of one Sheet laid at blocks
 * `ba` and `bb`, are the one node: every sector of each holds its sum where it
 * lies — the sum is seeded by the sector's address (§2.5), so the two copies
 * differ in it and only in it — and every byte past the sum is the other's. */
static bool StandTipInCopiesOne(const uint8_t* a, uint64_t ba, const uint8_t* b, uint64_t bb)
{
    const uint8_t* uuid = tagfs_get_state()->uuid;
    uint32_t       sum  = sizeof(((const BookSectorHead*)0)->crc32c);
    for (uint32_t k = 0; k < TAGFS_BLOCK_SECTORS; k++) {
        const uint8_t* sa = a + (uint64_t)k * BOOK_SECTOR_BYTES;
        const uint8_t* sb = b + (uint64_t)k * BOOK_SECTOR_BYTES;
        if (!BookSectorCheck(sa, uuid, ba * TAGFS_BLOCK_SECTORS + k) ||
            !BookSectorCheck(sb, uuid, bb * TAGFS_BLOCK_SECTORS + k) ||
            memcmp(sa + sum, sb + sum, BOOK_SECTOR_BYTES - sum) != 0)
            return false;
    }
    return true;
}

/* The leaf of FILES of the Tally stood on whose keys begin at `lo`. */
static bool StandTipInLeafAt(TallyKey lo, TallyPlace* out)
{
    for (uint32_t i = 0; TallyStandNodeWhere(TALLY_FILES, i, out); i++)
        if (out->level == 0 && out->has_lo && out->lo.k1 == lo.k1 && out->lo.k2 == lo.k2)
            return true;
    return false;
}

static TestResult TestStandTipInHealsFiles(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    MountBookSaid      said;
    TallyPlace         leaf, again;
    bool               found = false;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandTipInMake(1000u, STAND_TIP_IN_MANY), "files enough for FILES to take three leaves");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    for (uint32_t i = 1; !found && TallyStandNodeWhere(TALLY_FILES, i, &leaf); i++)
        found = leaf.level == 0 && leaf.has_lo && leaf.has_hi;
    TEST_ASSERT(found && leaf.ptr.copies == 2, "a leaf of FILES between two others, two copies");
    TEST_ASSERT(StandBinderKeep(&b, leaf.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, leaf.ptr.block[0], g_test_output), "and written over with zeros");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.mended >= 1 && said.rotten == 0, "the leaf read through its second copy, its first to mend");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.mended, 0, "every copy of every node holds (C24)");
    TEST_ASSERT(StandTipInLeafAt(leaf.lo, &again), "a leaf begins where the mended one began");
    TEST_ASSERT(again.ptr.seq == StandTipInStood() && again.ptr.block[0] != leaf.ptr.block[0],
                "laid anew by the Tally, elsewhere: no keys of it changed, its copy did (§16.2 п.4)");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* The leaf of the reads' DIGESTS table that names `block`. */
static bool StandTipInDigestLeaf(uint64_t block, TallyPlace* out)
{
    bool rot = false;
    for (uint32_t i = 0; TallyDigestLeafAt(i, out, &rot); i++)
        if (!rot && out->level == 0 && (!out->has_lo || out->lo.k1 <= block) && (!out->has_hi || block < out->hi.k1))
            return true;
    return false;
}

static TestResult TestStandTipInHealsDigests(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    static uint8_t     second[TAGFS_BLOCK_SIZE];
    StandFreshModel    m;
    TallyPlace         leaf, again;
    FileRecord*        rec   = NULL;
    FileSpot           spot  = { 0, 0, 0 };
    uint64_t           fid   = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file("tipin-digest.bin", NULL, 0, &fid), 0, "a file for data is made");
    TEST_ASSERT(StandFreshModelMake(&m, 4u * TAGFS_BLOCK_SIZE) &&
                StandFreshWrite(&m, fid, 0, 3u * TAGFS_BLOCK_SIZE, 61) == 3 * TAGFS_BLOCK_SIZE, "and written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it is written: its DIGESTS names the data");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(rec && FileRecordFind(rec, 0, &spot) && spot.block, "the data's first block");
    TEST_ASSERT(StandTipInDigestLeaf(spot.block, &leaf) && leaf.ptr.copies == 2,
                "the DIGESTS leaf that names it, two copies");
    TEST_ASSERT(StandBinderKeep(&b, leaf.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, leaf.ptr.block[0], g_test_output), "and written over with zeros");
    FileRecordRelease(rec);
    rec = NULL;
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again: the leaves of its DIGESTS kept, none read");
    tagfs_readahead_invalidate(spot.block);
    TEST_ASSERT(StandFreshHolds(&m, fid), "the data reads back: its digest read through the leaf's second copy");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(StandTipInDigestLeaf(spot.block, &again), "a leaf of its DIGESTS names the block");
    TEST_ASSERT(again.ptr.seq == StandTipInStood() && again.ptr.block[0] != leaf.ptr.block[0],
                "laid anew by the Tally, elsewhere: the leaf read lazily was marked by its read (C24)");
    TEST_ASSERT(StandBinderRead(&b, again.ptr.block[0], g_test_output) && g_test_output[0] | g_test_output[1],
                "its first copy is on the medium");
    TEST_ASSERT(again.ptr.copies == 2 && StandBinderRead(&b, again.ptr.block[1], second) &&
                StandTipInCopiesOne(g_test_output, again.ptr.block[0], second, again.ptr.block[1]),
                "and its second, the same node: every sector of each holds at its own place, every byte past its "
                "sum the other's — every copy holds after it (C24)");
    tagfs_readahead_invalidate(spot.block);
    TEST_ASSERT(StandFreshHolds(&m, fid), "and the data reads");
out:
    FileRecordRelease(rec);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A volume filled and emptied by Tallies laid in: its ground back
// ----------------------------------------------------------------------------

#define STAND_TIP_IN_FILLED   6000u    /* files: FILES of tens of leaves, its Tallies past one Quire with DIGESTS */
#define STAND_TIP_IN_FILL_MB  4u

/* Every block the slots hold now — their Tallies' Quires and nodes, their
 * ribbons' Quires. */
static uint64_t StandTipInHeld(void)
{
    static Strip ground[1024];
    uint32_t     n = BinderStandGround(ground, 1024u);
    uint64_t     blocks = 0;
    for (uint32_t i = 0; i < n && i < 1024u; i++)
        blocks += ground[i].count;
    return n <= 1024u ? blocks : UINT64_MAX;
}

static TestResult TestStandTipInEmptied(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    char               name[80];
    uint64_t           first = 0, fid = 0, data = 0, free0 = 0, held0 = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    uint64_t q = RibbonCursorNow()->quire_blocks;
    TEST_ASSERT_OK(StandBinderTallies(3u), "the ring filled with Tallies of the volume as found");
    free0 = tagfs_get_state()->ledger.free_blocks;
    held0 = StandTipInHeld();
    TEST_ASSERT(held0 != UINT64_MAX, "the slots' ground counted");
    for (uint32_t i = 0; i < STAND_TIP_IN_FILLED; i++) {
        ksnprintf(name, sizeof(name), "tipin-fill-%05u.n", i);
        TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, &fid), 0, "a file is made");
        first = first ? first : fid;
        if (i % 1000u == 999u)
            TEST_ASSERT_OK(TagFSSettle(), "the files settle a batch at a time");
    }
    TEST_ASSERT_EQ(tagfs_create_file("tipin-fill.bin", NULL, 0, &data), 0, "a file for data is made");
    TEST_ASSERT(StandFreshModelMake(&m, STAND_TIP_IN_FILL_MB * 1024u * 1024u), "room for its bytes");
    for (uint32_t k = 0; k < STAND_TIP_IN_FILL_MB * 256u; k += 64u)
        TEST_ASSERT_EQ(StandFreshWrite(&m, data, (uint64_t)k * TAGFS_BLOCK_SIZE, 64u * TAGFS_BLOCK_SIZE, 300u + k),
                       64 * TAGFS_BLOCK_SIZE, "its bytes written");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally of them is written");
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounts on it, a stitch of it settled");
    for (uint64_t f = first; f < first + STAND_TIP_IN_FILLED / 2u; f++) {
        TEST_ASSERT_EQ(tagfs_delete_file(f, INK_UNWARRANTED), 0, "half the files given up");
        if ((f - first) % 1000u == 999u)
            TEST_ASSERT_OK(TagFSSettle(), "the deletes settle a batch at a time");
    }
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally laid in over the half given up");
    for (uint64_t f = first + STAND_TIP_IN_FILLED / 2u; f < first + STAND_TIP_IN_FILLED; f++) {
        TEST_ASSERT_EQ(tagfs_delete_file(f, INK_UNWARRANTED), 0, "the other half given up");
        if ((f - first) % 1000u == 999u)
            TEST_ASSERT_OK(TagFSSettle(), "the deletes settle a batch at a time");
    }
    TEST_ASSERT_EQ(tagfs_delete_file(data, INK_UNWARRANTED), 0, "and the data");
    TEST_ASSERT_OK(TagFSSettle(), "every delete settled");
    TEST_ASSERT_OK(SettleForRoom(), "room is asked for: the ribbon leaves the Quire the deletes lie in");
    TEST_ASSERT_OK(StandBinderTallies(4u), "and Tallies of the emptied volume laid in over the whole ring");
    uint64_t free1 = tagfs_get_state()->ledger.free_blocks;
    uint64_t held1 = StandTipInHeld();
    TestEvidence("free blocks before the files and after them", free0);
    TestEvidence("free blocks after the files are given up", free1);
    TestEvidence("blocks the slots held before", held0);
    TestEvidence("blocks the slots hold after", held1);
    TEST_ASSERT(held1 != UINT64_MAX, "the slots' ground counted");
    TEST_ASSERT(free1 + held1 + 2u * q >= free0 + held0,
                "every block the test took is free again or held by the slots, but the ribbon's Quire and its "
                "next_quire: no Quire of the Tallies is left neither (§16.5 п.17)");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The ring written over the slot that laid a node: its Quire still held
// ----------------------------------------------------------------------------

#define STAND_TIP_IN_QUIRED   12000u   /* files: a whole Tally of FILES and TAGS past a Tally-Quire of 256 */

/* Whether every block of every copy of `p` lies in ground a slot holds. */
static bool StandTipInPlaceHeld(const TallyPlace* p, const Strip* ground, uint32_t n)
{
    for (uint32_t k = 0; k < p->ptr.copies; k++)
        for (uint64_t at = TallyPtrBlock(&p->ptr, k), end = at + p->ptr.sheets; at < end; at++) {
            bool held = false;
            for (uint32_t i = 0; !held && i < n; i++)
                held = ground[i].start <= at && at < ground[i].start + ground[i].count;
            if (!held)
                return false;
        }
    return true;
}

/* What the Tally a mount would stand on now names, against the ground the
 * slots hold: its nodes Tally `seq` laid — `older`, copy 1 of them from Quire
 * `qlo` to `qhi` — and the ones outside that ground, `loose`. */
typedef struct {
    uint32_t older;
    uint32_t loose;
    uint64_t qlo;
    uint64_t qhi;
} StandTipInNamed;

static void StandTipInNamedOne(StandTipInNamed* s, const TallyPlace* p, uint64_t seq, uint64_t q,
                               const Strip* ground, uint32_t n)
{
    uint64_t at = TallyPtrBlock(&p->ptr, 0) / q;
    s->loose += !StandTipInPlaceHeld(p, ground, n);
    if (p->ptr.seq != seq)
        return;
    s->older++;
    s->qlo = at < s->qlo ? at : s->qlo;
    s->qhi = at > s->qhi ? at : s->qhi;
}

/* Every node of FILES, TAGS and USE and every inner node of DIGESTS as the
 * Foliation holds them, and every DIGESTS leaf of the reads' table. */
static void StandTipInNamedAll(StandTipInNamed* s, uint64_t seq, uint64_t q)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    static Strip          ground[1024];
    uint32_t              n = BinderStandGround(ground, 1024u);
    FoliationNode         f;
    TallyPlace            p;
    bool                  rot = false;
    n = n < 1024u ? n : 1024u;
    *s = (StandTipInNamed){ 0, 0, UINT64_MAX, 0 };
    for (uint32_t t = 0; t < sizeof(trees) / sizeof(trees[0]); t++)
        for (uint32_t i = 0; FoliationStandNode(trees[t], i, &f); i++)
            StandTipInNamedOne(s, &f.place, seq, q, ground, n);
    for (uint32_t i = 0; TallyDigestLeafAt(i, &p, &rot); i++)
        StandTipInNamedOne(s, &p, seq, q, ground, n);
}

static TestResult TestStandTipInQuiresHeld(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    char               name[80];
    uint64_t           fid = 0, seq = 0, q = 0;
    StandTipInNamed    named;
    FoliationNode      f;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    for (uint32_t i = 0; i < STAND_TIP_IN_QUIRED; i++) {
        ksnprintf(name, sizeof(name), "tipin-quire-%05u-of-a-name-long-enough-to-take-room.txt", i);
        TEST_ASSERT_EQ(StandTipInCreate(name, &fid), 0, "a file is made");
    }
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "they settled, a Tally of them written");
    for (uint32_t i = 0; FoliationStandNode(TALLY_FILES, i, &f); i++)
        seq = f.place.ptr.seq > seq ? f.place.ptr.seq : seq;
    q = RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u;
    for (uint32_t k = 0; k <= g_binder.ring; k++) {
        ksnprintf(name, sizeof(name), "tipin-quire-tail-%u.txt", k);
        TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, &fid), 0, "a file more is made at the tail");
        TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a Tally laid in over it");
    }
    StandTipInNamedAll(&named, seq, q);
    TestEvidence("nodes the first Tally laid that the last names as they lie", named.older);
    TestEvidence("Tally-Quires copy 1 of them spans", named.qhi >= named.qlo ? named.qhi - named.qlo + 1u : 0);
    TEST_ASSERT(named.older > 0 && named.qhi > named.qlo, "the last Tally names nodes the first laid, over more "
                "than one Quire, its slot written over by the ring");
    TestEvidence("nodes the last Tally names outside the ground the slots hold", named.loose);
    TEST_ASSERT(named.loose == 0, "every node the last Tally names lies in ground a slot holds: a Quire is held "
                "while a node in it lives (§16.5 п.17)");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The last leaf of DIGESTS rotten: what is born past it laid past it
// ----------------------------------------------------------------------------

#define STAND_TIP_IN_DATA_MB    4u       /* DIGESTS of two leaves and more */
#define STAND_TIP_IN_PAST_MOST  4096u    /* blocks the file after it is written to at most, ... */
#define STAND_TIP_IN_PAST_STEP  64u      /* ... so many a write, until one lies past the rotten leaf */

/* The last leaf of the reads' DIGESTS table: the one with no upper key. */
static bool StandTipInDigestLast(TallyPlace* out)
{
    TallyPlace p;
    bool       rot = false, found = false;
    for (uint32_t i = 0; TallyDigestLeafAt(i, &p, &rot); i++)
        if (p.level == 0 && !p.has_hi) {
            *out  = p;
            found = true;
        }
    return found;
}

static TestResult TestStandTipInRottenPast(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    data, past;
    TallyPlace         last;
    FileRecord*        rec   = NULL;
    FileSpot           spot  = { 0, 0, 0 };
    uint64_t           fid = 0, pfid = 0, at = 0;
    bool               beyond = false;
    const char*        aside = StandBinderAside(&b);

    memset(&data, 0, sizeof(data));
    memset(&past, 0, sizeof(past));
    memset(&last, 0, sizeof(last));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file("tipin-rot-data.bin", NULL, 0, &fid), 0, "a file for data is made");
    TEST_ASSERT(StandFreshModelMake(&data, (uint64_t)STAND_TIP_IN_DATA_MB << 20) &&
                StandFreshWrite(&data, fid, 0, STAND_TIP_IN_DATA_MB << 20, 71) == (int)(STAND_TIP_IN_DATA_MB << 20),
                "and written: DIGESTS takes leaves");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "settled, a Tally of it written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(StandTipInDigestLast(&last) && last.has_lo && last.ptr.copies == 2,
                "the last leaf of DIGESTS, from a key of its own and to none, two copies");
    TEST_ASSERT(StandBinderKeep(&b, last.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, last.ptr.block[0], g_test_output) &&
                StandBinderWrite(&b, last.ptr.block[1], g_test_output), "both its copies written over with zeros");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again: the leaf rots when a read reaches it");
    TEST_ASSERT_EQ(tagfs_create_file("tipin-rot-past.bin", NULL, 0, &pfid), 0, "a file is made after");
    TEST_ASSERT(StandFreshModelMake(&past, (uint64_t)STAND_TIP_IN_PAST_MOST * TAGFS_BLOCK_SIZE), "its model");
    for (at = 0; !beyond && at < STAND_TIP_IN_PAST_MOST; at += STAND_TIP_IN_PAST_STEP) {
        TEST_ASSERT(StandFreshWrite(&past, pfid, at * TAGFS_BLOCK_SIZE, STAND_TIP_IN_PAST_STEP * TAGFS_BLOCK_SIZE,
                                    73) == (int)(STAND_TIP_IN_PAST_STEP * TAGFS_BLOCK_SIZE), "written on");
        rec = FileMapRecordHold(tagfs_get_state()->files, pfid, NULL);
        TEST_ASSERT(rec && FileRecordFind(rec, at + STAND_TIP_IN_PAST_STEP - 1u, &spot) && spot.block,
                    "the last block written");
        beyond = spot.block > last.lo.k1;
        FileRecordRelease(rec);
        rec = NULL;
    }
    TestEvidence("blocks written until one lay past the rotten leaf's first", at);
    TEST_ASSERT(beyond, "a block of it lies past the rotten leaf's first key");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(StandFreshHolds(&past, pfid), "every block of it reads: what was born past the rotten leaf is laid "
                "past it, the leaf named as it lies (§16.4 п.5)");
out:
    FileRecordRelease(rec);
    StandFreshModelLetGo(&past);
    StandFreshModelLetGo(&data);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_tip_in_tests[] = {
    { .name = "stand_tip_in_parcel", .func = TestStandTipInParcel,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_parcels_as_they_lie", .func = TestStandTipInParcelsAsTheyLie,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_heals_files", .func = TestStandTipInHealsFiles,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_heals_digests", .func = TestStandTipInHealsDigests,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_emptied", .func = TestStandTipInEmptied,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_quires_held", .func = TestStandTipInQuiresHeld,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tip_in_rotten_past", .func = TestStandTipInRottenPast,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_tip_in_suite = {
    g_stand_tip_in_tests, sizeof(g_stand_tip_in_tests) / sizeof(g_stand_tip_in_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
