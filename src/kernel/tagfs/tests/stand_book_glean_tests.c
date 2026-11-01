#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/foliation.h"
#include "../settle/settle.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../mount/ribbon.h"
#include "../file_map/file_map.h"
#include "../glean/glean.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_book_glean suite: what a book's Tallies keep of their ground
// (spec_v2 §4.8, §2.5, §16.5 пп.17–18)
//
// A logger's Tallies are laid in among the nodes of the ones before them, and
// a node nothing changed is named as it lies wherever it was laid: left to
// itself the book would hold a whole Tally-Quire for one node in it, Tally
// after Tally. Glean holds the Tally-Quires to GLEAN_SLACK times the blocks
// the live nodes take and the current pair — the nodes of the emptiest Quire
// laid again by the next Tally, the Quire going back whole once no slot names
// a node in it. And a copy of a leaf no Tally lays again is still read: each
// Tally laid in reads both copies of as many of the leaves checked longest ago
// as it lays, and a copy that does not hold is laid anew — the second copy a
// mount never reads when the first holds rots no more in silence (C24, Р-e3).
// Every test puts the Spine and the ribbon back as it found them.
// ============================================================================

#define STAND_BOOK_GLEAN_FILES   200u    /* files of a long name: FILES of leaves past the logger's */
#define STAND_BOOK_GLEAN_MOST    600u    /* the logger's Tallies at most, ... */
#define STAND_BOOK_GLEAN_MOVES   3u      /* ... until the pair of Tally-Quires moved on so many times */
#define STAND_BOOK_COPY_FILES    320u    /* FILES of five leaves and more */

/* `n` files of a long name made from `from` on, settled. */
static bool StandBookGleanMake(const char* stem, uint32_t n)
{
    char     name[96];
    uint64_t fid = 0;
    for (uint32_t i = 0; i < n; i++) {
        ksnprintf(name, sizeof(name), "%s-%05u-of-a-name-long-enough-to-fill-leaves.txt", stem, i);
        if (tagfs_create_file(name, NULL, 0, &fid) != 0)
            return false;
    }
    return TagFSSettle() == OK;
}

/* One line more of the logger — a block of its own at `line` — settled, and
 * a Tally laid in over it with the lag's stitches past it. */
static bool StandBookGleanLine(StandFreshModel* m, uint64_t fid, uint32_t line)
{
    return StandFreshWrite(m, fid, (uint64_t)line * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 500u + line) ==
               (int)TAGFS_BLOCK_SIZE &&
           TagFSSettle() == OK && StandBinderTallies(1u) == OK;
}

/* The Quires of the grid of `q` blocks that [start, start + count) meets,
 * marked in `bit` (`nquire` of them). */
static void StandBookGleanMark(uint64_t* bit, uint64_t nquire, uint64_t q, uint64_t start, uint64_t count)
{
    for (uint64_t at = start / q; count && at < nquire && at * q < start + count; at++)
        bit[at / 64u] |= 1ull << (at % 64u);
}

/* Every Quire a slot that holds names a node in — its own nodes and the
 * Quires its Tally names a node in as it lies — and the current pair: the
 * blocks they take, a Quire whole; UINT64_MAX with no memory. */
static uint64_t StandBookGleanHeld(uint64_t q, uint64_t** marks, uint64_t* nquire)
{
    uint64_t n = (tagfs_get_state()->block_end + q - 1u) / q, blocks = 0;
    uint64_t* bit = kmalloc(sizeof(uint64_t) * (n / 64u + 1u));
    if (!bit)
        return UINT64_MAX;
    memset(bit, 0, sizeof(uint64_t) * (n / 64u + 1u));
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        for (uint32_t k = 0; s->holds && k < s->nground; k++)
            StandBookGleanMark(bit, n, q, s->ground[k].start, s->ground[k].count);
        for (uint32_t k = 0; s->holds && k < s->nquires; k++)
            StandBookGleanMark(bit, n, q, s->quires[k].start, s->quires[k].count);
    }
    StandBookGleanMark(bit, n, q, g_binder.pair_a, g_binder.pair_room);
    StandBookGleanMark(bit, n, q, g_binder.pair_b, g_binder.pair_room);
    for (uint64_t at = 0; at < n; at++)
        blocks += bit[at / 64u] & (1ull << (at % 64u)) ? q : 0;
    *marks  = bit;
    *nquire = n;
    return blocks;
}

/* The blocks every copy of every node of the Tally the next one is laid
 * against takes, in Sheets: the live nodes. */
static uint64_t StandBookGleanLive(void)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    FoliationNode         f;
    TallyPlace            p;
    bool                  rot = false;
    uint64_t              live = 0;
    for (uint32_t t = 0; t < sizeof(trees) / sizeof(trees[0]); t++)
        for (uint32_t i = 0; FoliationStandNode(trees[t], i, &f); i++)
            live += f.place.ptr.sheets * f.place.ptr.copies;
    for (uint32_t i = 0; TallyDigestLeafAt(i, &p, &rot); i++)
        live += p.level == 0 && !rot ? p.ptr.sheets * p.ptr.copies : 0;
    return live;
}

/* Whether every block of the logger's first `lines` lies off the Quires
 * marked: a Tally-Quire goes back to the map whole, or not at all. */
static bool StandBookGleanOff(uint64_t fid, uint32_t lines, const uint64_t* bit, uint64_t nquire, uint64_t q)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    FileSpot    spot;
    bool        off = rec != NULL;
    for (uint32_t i = 0; off && i < lines; i++) {
        uint64_t at = FileRecordFind(rec, i, &spot) && spot.block ? spot.block / q : UINT64_MAX;
        off = at != UINT64_MAX && (at >= nquire || !(bit[at / 64u] & (1ull << (at % 64u))));
    }
    FileRecordRelease(rec);
    return off;
}

// ----------------------------------------------------------------------------
// A logger's Tallies: the Tally-Quires within GLEAN_SLACK times the live
// ----------------------------------------------------------------------------

static TestResult TestStandBookGleanQuires(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, q = 0, pair = 0, held = 0, live = 0, nquire = 0;
    uint64_t*          marks = NULL;
    uint32_t           lines = 0, moves = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandBookGleanMake("glean", STAND_BOOK_GLEAN_FILES), "files whose nodes no line changes");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written");
    TEST_ASSERT_EQ(tagfs_create_file("glean-logger.log", NULL, 0, &fid), 0, "the logger's file is made");
    TEST_ASSERT(StandFreshModelMake(&m, (uint64_t)STAND_BOOK_GLEAN_MOST * TAGFS_BLOCK_SIZE), "room for its lines");
    q    = RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u;
    pair = g_binder.pair_a;
    for (lines = 0; lines < STAND_BOOK_GLEAN_MOST && moves < STAND_BOOK_GLEAN_MOVES; lines++) {
        TEST_ASSERT(StandBookGleanLine(&m, fid, lines), "a line written, settled, a Tally laid in over it");
        moves += g_binder.pair_a != pair;
        pair   = g_binder.pair_a;
    }
    TestEvidence("the logger's Tallies", lines);
    TestEvidence("the pair of Tally-Quires moved on", moves);
    TEST_ASSERT(moves >= STAND_BOOK_GLEAN_MOVES, "the Tallies went through Tally-Quires");
    TEST_ASSERT_OK(StandBinderTallies(g_binder.ring + 2u), "and the ring written over twice and more");
    held = StandBookGleanHeld(q, &marks, &nquire);
    live = StandBookGleanLive();
    TestEvidence("blocks of the Tally-Quires the slots and the pair hold", held);
    TestEvidence("blocks the live nodes take, every copy", live);
    TestEvidence("blocks the current pair takes", 2u * g_binder.pair_room);
    TEST_ASSERT(held != UINT64_MAX, "the Tally-Quires counted");
    TEST_ASSERT(held <= GLEAN_SLACK * live + 2u * g_binder.pair_room,
                "the Tally-Quires stay within GLEAN_SLACK times the live nodes, and the current pair (§4.8)");
    TEST_ASSERT(StandBookGleanOff(fid, lines, marks, nquire, q),
                "no line lies in a Tally-Quire a slot holds: a Quire goes back whole");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and the logger reads every line written");
out:
    kfree(marks);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The second copy of a leaf no Tally lays again: read, and laid anew
// ----------------------------------------------------------------------------

/* The leaf of FILES the Foliation holds whose keys begin at `lo`. */
static bool StandBookGleanLeafAt(TallyKey lo, TallyPlace* out)
{
    FoliationNode f;
    for (uint32_t i = 0; FoliationStandNode(TALLY_FILES, i, &f); i++)
        if (f.place.level == 0 && f.place.has_lo && f.place.lo.k1 == lo.k1 && f.place.lo.k2 == lo.k2) {
            *out = f.place;
            return true;
        }
    return false;
}

/* The leaves of every tree the Tally the next one is laid against holds. */
static uint32_t StandBookGleanLeaves(void)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE };
    FoliationNode         f;
    TallyPlace            p;
    bool                  rot = false;
    uint32_t              n = 0;
    for (uint32_t t = 0; t < sizeof(trees) / sizeof(trees[0]); t++)
        for (uint32_t i = 0; FoliationStandNode(trees[t], i, &f); i++)
            n += f.place.level == 0;
    for (uint32_t i = 0; TallyDigestLeafAt(i, &p, &rot); i++)
        n += p.level == 0;
    return n;
}

/* Whether a block reads as anything but zeros. */
static bool StandBookGleanWritten(const StandBinder* b, uint64_t block)
{
    if (!StandBinderRead(b, block, g_test_output))
        return false;
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++)
        if (g_test_output[i])
            return true;
    return false;
}

/* Leaf `n` of FILES (0 the first) the Tally the next one is laid against
 * holds, by the Foliation — the Tally the Binder just wrote, no mount since. */
static bool StandBookGleanLeafNth(uint32_t n, TallyPlace* out)
{
    FoliationNode f;
    for (uint32_t i = 0; FoliationStandNode(TALLY_FILES, i, &f); i++)
        if (f.place.level == 0 && n-- == 0) {
            *out = f.place;
            return true;
        }
    return false;
}

/*
 * ‼ ONLY THE COPY CHECK LAYS THE LEAF ANEW HERE. The Tally of the files is
 * written whole in the pair of Tally-Quires the mount took, and the Tallies a
 * Glean asks for are laid before the leaf is spoiled: on a book whose
 * Tally-Quires earlier Tallies thinned — every boot of a stand past its first
 * — that Tally's DONE may choose one to glean, and a Tally of it would lay
 * the clean leaf anew with its live nodes. No mount follows: every Tally laid
 * in after them stays in that pair, so Glean counts no Quire past it
 * (binder_glean.c) and nothing more gathers the leaf. The logger's
 * file lies past every file of the leaf, its lines change no key of it, and
 * the leaf after it is clean too: a Tally that lays the spoiled leaf anew and
 * not its clean neighbour did so because a copy of it was found wanting —
 * with no copy read (book_proofblind) none ever is.
 */
static TestResult TestStandBookGleanCopies(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    TallyPlace         leaf, next, again, beside;
    uint64_t           fid = 0;
    uint32_t           tallies = 0, most = 0;
    bool               laid = false;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandBookGleanMake("proof", STAND_BOOK_COPY_FILES), "files enough for FILES to take five leaves");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written whole, in the pair of Tally-Quires the mount took");
    TEST_ASSERT(StandBinderGleaned(), "the Tallies Glean asks for laid first");
    TEST_ASSERT(StandBookGleanLeafNth(1, &leaf) && StandBookGleanLeafNth(2, &next) && next.has_hi,
                "FILES holds a second leaf and a third with leaves past it");
    TEST_ASSERT(leaf.ptr.copies == 2, "the second leaf has two copies");
    TEST_ASSERT(StandBinderKeep(&b, TallyPtrBlock(&leaf.ptr, 1)), "its second copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, TallyPtrBlock(&leaf.ptr, 1), g_test_output),
                "and written over with zeros: no Tally of the leaf reads it, and none writes it");
    TEST_ASSERT_EQ(tagfs_create_file("proof-logger.log", NULL, 0, &fid), 0, "a logger's file is made after them");
    most = StandBookGleanLeaves() + 8u;
    TEST_ASSERT(StandFreshModelMake(&m, (uint64_t)most * TAGFS_BLOCK_SIZE), "room for its lines");
    for (tallies = 0; !laid && tallies < most; tallies++) {
        TEST_ASSERT(StandBookGleanLine(&m, fid, tallies), "a line written, settled, a Tally laid in over it");
        laid = StandBookGleanLeafAt(leaf.lo, &again) && again.ptr.seq != leaf.ptr.seq;
    }
    TestEvidence("the leaves the Tally holds", most - 8u);
    TestEvidence("Tallies laid in until the leaf was laid anew", tallies);
    TEST_ASSERT(laid, "a Tally laid in read both copies of the leaf no line changed, and laid it anew (§16.5 п.18)");
    TEST_ASSERT(StandBookGleanLeafAt(next.lo, &beside) && beside.ptr.seq == next.ptr.seq,
                "and the clean leaf after it as it lay: the copy found wanting laid it, not Glean or a whole Tally");
    TEST_ASSERT(again.ptr.copies == 2 && TallyPtrBlock(&again.ptr, 1) != TallyPtrBlock(&leaf.ptr, 1),
                "elsewhere: nothing is written in place (§2.5)");
    TEST_ASSERT(StandBookGleanWritten(&b, TallyPtrBlock(&again.ptr, 0)) &&
                StandBookGleanWritten(&b, TallyPtrBlock(&again.ptr, 1)), "both its new copies on the medium");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "the volume mounts again, the logger reads");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_book_glean_tests[] = {
    { .name = "stand_book_glean_quires", .func = TestStandBookGleanQuires,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_book_glean_copies", .func = TestStandBookGleanCopies,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_book_glean_suite = {
    g_stand_book_glean_tests, sizeof(g_stand_book_glean_tests) / sizeof(g_stand_book_glean_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
