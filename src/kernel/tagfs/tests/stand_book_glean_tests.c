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
#define STAND_BOOK_COPY_ROUNDS   4u      /* rounds of the copy check's sweep the logger's lines go at the most */

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

/* The leaves of `tree` the Tally the next one is laid against holds. */
static uint32_t StandBookGleanTreeLeaves(uint64_t tree)
{
    FoliationNode f;
    uint32_t      n = 0;
    for (uint32_t i = 0; FoliationStandNode(tree, i, &f); i++)
        n += f.place.level == 0;
    return n;
}

/* The leaves of every tree the Tally the next one is laid against holds: the
 * ring the copy check's sweep goes round (binder_proof.c). */
static uint32_t StandBookGleanLeaves(void)
{
    TallyPlace p;
    bool       rot = false;
    uint32_t   n   = StandBookGleanTreeLeaves(TALLY_FILES) + StandBookGleanTreeLeaves(TALLY_TAGS) +
                     StandBookGleanTreeLeaves(TALLY_USE);
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

/* Whether leaf `p` of FILES begins at a file of the test's own `stem`. */
static bool StandBookGleanOwn(const TallyPlace* p, const char* stem)
{
    FileRecord* rec = p->has_lo ? FileMapRecordHold(tagfs_get_state()->files, p->lo.k1, NULL) : NULL;
    bool        own = rec && strncmp(rec->name, stem, strlen(stem)) == 0;
    FileRecordRelease(rec);
    return own;
}

/* The leaf the test spoils and the clean one past it, as the Tally the next
 * one is laid against holds them: four from the end of FILES, of the test's
 * own files, two copies — and packed full, the one past it laid by the same
 * Tally. Files made since lie past every key the book held, so no node named
 * as it lies parts two leaves of them: a leaf the row runs on past was filled
 * until it was full (binder_nodes.c, LevelStep). */
static bool StandBookGleanChosen(TallyPlace* leaf, TallyPlace* next)
{
    uint32_t leaves = StandBookGleanTreeLeaves(TALLY_FILES);
    return leaves >= 5u && StandBookGleanLeafNth(leaves - 4u, leaf) && StandBookGleanLeafNth(leaves - 3u, next) &&
           StandBookGleanOwn(leaf, "proof-") && next->has_hi && leaf->ptr.copies == 2 &&
           next->ptr.seq == leaf->ptr.seq;
}

/* The first block of the second copy of `leaf` written over with zeros. */
static bool StandBookGleanSpoil(const StandBinder* b, const TallyPlace* leaf)
{
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    return StandBinderWrite(b, TallyPtrBlock(&leaf->ptr, 1), g_test_output);
}

/* The Tallies a Glean asked for at the last Tally's DONE laid
 * (StandBinderGleaned). The spoiled leaf as it lay, the clean one past it is
 * taken again by its keys; laid anew by them — its keys where they were, or
 * a Tally written whole packing them afresh — the leaf and the one past it
 * are chosen again and the leaf spoiled in its new second copy: its block
 * lies on ground laid since the ring was kept, which the put-back gives back
 * as the ring was found — the slots' ground as kept, a Quire taken since free
 * (StandBinderPutBack). False when a Tally was refused or one is owed still,
 * or the two leaves are not found again. */
static bool StandBookGleanPast(const StandBinder* b, TallyPlace* leaf, TallyPlace* next)
{
    BinderStandSaid said;
    TallyPlace      now, past;
    BinderStand(&said);
    if (!said.demanded)
        return true;
    if (!StandBinderGleaned())
        return false;
    if (StandBookGleanLeafAt(leaf->lo, &now) && now.ptr.seq == leaf->ptr.seq) {
        if (!StandBookGleanLeafAt(next->lo, &past))
            return false;
        *next = past;
        return true;
    }
    kprintf("[TESTS] stand_book_glean_copies: the Tallies a Glean asked for laid the spoiled leaf anew — the leaf "
            "is chosen again and spoiled again\n");
    return StandBookGleanChosen(leaf, next) && StandBookGleanSpoil(b, leaf);
}

/*
 * ‼ ONLY THE COPY CHECK LAYS THE LEAF ANEW IN A LINE'S TALLY HERE. The Tally
 * of the files lays the nodes they changed in the pair of Tally-Quires the
 * mount took and names every other as it lies, where earlier Tallies laid it;
 * at its DONE — or at a line's, once the lines fill the pair and it moves on,
 * the pair the test's files lie in then past it — Glean may choose a Quire,
 * and the Tally after lays every live node of it anew, or the whole Tally
 * when that costs no more (binder_glean.c; binder_kept.c, BinderTipInTake):
 * the spoiled leaf and its clean neighbour as like as not, no copy of either
 * read. So the Tallies a Glean asks for are laid before each line, and a leaf
 * they laid anew is chosen again and spoiled again (StandBookGleanPast) — a
 * copy found wanting at the DONE that chose the Glean is not counted then.
 * The logger's file lies past every file of the leaf, its lines change no key
 * of it nor of the leaf after it: a line's Tally that lays the spoiled leaf
 * anew and not its clean neighbour did so because a copy of it was found
 * wanting — with no copy read (book_proofblind) none ever is, and the lines
 * run out. The leaf spoiled is one packed full, four from the end of FILES: a
 * row of leaves laid anew that comes to less than half a Sheet takes its
 * margin in (tip_in.c, LeavesSifted; binder_nodes.c, RowEnded) — on a used
 * book an early leaf thinned by the files given up since would take the clean
 * leaf after it in with it — and the logger's leaf, the last, may take in the
 * one before it, never the one before that. A line's Tally lays a leaf at the
 * least and the copy check reads as many as it lays, so the sweep goes round
 * the leaves in as many lines as there are, and a few for its turns from tree
 * to tree (binder_proof.c, ProofNext): the lines go round once to reach the
 * leaf, and once more for each Glean laid in that lays it anew and passes it
 * in the sweep as a leaf of its own — a whole Tally reads no copy and moves
 * the sweep not at all — STAND_BOOK_COPY_ROUNDS rounds at the most.
 */
static TestResult TestStandBookGleanCopies(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    TallyPlace         leaf, next, again, beside;
    uint64_t           fid = 0;
    uint32_t           tallies = 0, round = 0;
    bool               laid = false;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandBookGleanMake("proof", STAND_BOOK_COPY_FILES), "files enough for FILES to take five leaves");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written, their nodes in the pair of Tally-Quires the mount took");
    TEST_ASSERT(StandBinderGleaned(), "the Tallies Glean asks for laid first");
    TEST_ASSERT(StandBookGleanChosen(&leaf, &next),
                "FILES holds a leaf of the test's own files four from its end, two copies, packed full: the one past "
                "it laid with it");
    TEST_ASSERT(StandBinderKeep(&b, TallyPtrBlock(&leaf.ptr, 1)), "its second copy's first block kept as found");
    TEST_ASSERT(StandBookGleanSpoil(&b, &leaf),
                "and written over with zeros: no Tally of the leaf reads it, and none writes it");
    TEST_ASSERT_EQ(tagfs_create_file("proof-logger.log", NULL, 0, &fid), 0, "a logger's file is made after them");
    round = StandBookGleanLeaves() + 8u;
    TEST_ASSERT(StandFreshModelMake(&m, (uint64_t)STAND_BOOK_COPY_ROUNDS * round * TAGFS_BLOCK_SIZE),
                "room for its lines");
    for (tallies = 0; !laid && tallies < STAND_BOOK_COPY_ROUNDS * round; tallies++) {
        TEST_ASSERT(StandBookGleanPast(&b, &leaf, &next),
                    "the Tallies a Glean asks for laid before the line, the leaf and the one past it found again");
        TEST_ASSERT(StandBookGleanLine(&m, fid, tallies), "a line written, settled, a Tally laid in over it");
        laid = StandBookGleanLeafAt(leaf.lo, &again) && again.ptr.seq != leaf.ptr.seq;
    }
    TestEvidence("the leaves the Tally holds", round - 8u);
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
