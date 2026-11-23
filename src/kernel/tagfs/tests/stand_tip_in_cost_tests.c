#include "stand_binder.h"
#include "../settle/binder.h"
#include "../settle/dog_ear.h"
#include "../settle/foliation.h"
#include "../settle/tip_in.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_tip_in suite, its cost: what the next Tally is reckoned at, held
// to the rule as the test counts it —
// every leaf a span DogEar holds meets, at its Sheets where a key of the state
// lies in its range and at none where none does, and every inner node over
// one, once; both copies and the slot, never past the last Tally's whole book.
// The test counts it off the Foliation, the reads' table and DogEar's spans as
// they stand, a key known live by the file map, the registry, the Use Context
// and every live file's ground, walked here — not by the Binder's walk
// (binder_cost.c, binder_live.c). And a Tally under way when the volume is let
// go of: the let-go weighs nothing (binder_slots.c, BinderMountReset).
// ============================================================================

#define STAND_COST_SPANS  512u   /* spans of one tree DogEar holds, at the most a count takes */

static DogEarSpan s_cost_spans[STAND_COST_SPANS];

/* Whether a live file holds a block of [lo, hi). */
static bool StandCostGround(TagFSVolume* vol, uint64_t lo, uint64_t hi)
{
    FileMap* map = vol->state.files;
    uint64_t fid = 0;
    for (uint64_t from = 0; FileMapFiles(map, from, &fid, 1) == 1; from = fid + 1u) {
        FileRecord* rec = FileMapRecordHold(map, fid, NULL);
        StripWalk   w;
        Strip       s;
        bool        in = false;
        if (!rec)
            continue;
        StripWalkBegin(rec, &w);
        while (!in && StripWalkNextGround(&w, &s))
            in = s.start < hi && s.start + s.count > lo;
        FileRecordRelease(rec);
        if (in)
            return true;
    }
    return false;
}

/* Whether a live file has a key in [lo, hi) of FILES — none for `lo` from the
 * tree's first key, for `hi` to its end: its header lies there, or the range
 * begins inside it. */
static bool StandCostFiles(TagFSVolume* vol, const TallyKey* lo, const TallyKey* hi)
{
    FileMap* map = vol->state.files;
    uint64_t fid = 0;
    for (uint64_t from = lo ? lo->k1 : 0; FileMapFiles(map, from, &fid, 1) == 1; from = fid + 1u) {
        TallyKey head = { fid, 0 };
        if (hi && !TipInKeyLess(TALLY_FILES, head, *hi))
            return lo && lo->k1 == fid && lo->k2 > 0;
        if ((lo && lo->k1 == fid && lo->k2 > 0) || !lo || !TipInKeyLess(TALLY_FILES, head, *lo))
            return true;
    }
    return false;
}

/* Whether a key of the state lies in [lo, hi) of `tree`, counted here. */
static bool StandCostKeeps(TagFSVolume* vol, uint64_t tree, const TallyKey* lo, const TallyKey* hi)
{
    TagRolled   rolled;
    UseText*    u    = NULL;
    bool        live = false;
    uint64_t    a    = lo && lo->k1 > vol->book.frame.data_block ? lo->k1 : vol->book.frame.data_block;
    uint64_t    z    = hi && hi->k1 < vol->book.data_end ? hi->k1 : vol->book.data_end;
    switch (tree) {
    case TALLY_FILES:
        return StandCostFiles(vol, lo, hi);
    case TALLY_TAGS:
        return TagRoll(vol->state.registry, lo ? (uint32_t)lo->k1 : 0,
                       hi && hi->k1 < UINT32_MAX ? (uint32_t)hi->k1 : UINT32_MAX, &rolled);
    case TALLY_USE:
        u    = tagfs_recall_use_context(vol, NULL);
        live = u && u->bytes;
        UseTextDrop(u);
        return live;
    default:
        return a < z && StandCostGround(vol, a, z);
    }
}

static bool StandCostLeaf(TagFSVolume* vol, uint64_t tree, uint32_t i, TallyPlace* p)
{
    bool rotten = false;
    return tree == TALLY_DIGESTS ? TallyDigestLeafAt(vol, i, p, &rotten) : FoliationLeafAt(vol, tree, i, p);
}

/* Whether a span of `s` (`n`) meets [lo, hi). */
static bool StandCostMet(uint64_t tree, uint32_t n, const TallyKey* lo, const TallyKey* hi)
{
    for (uint32_t k = 0; k < n; k++)
        if ((!hi || TipInKeyLess(tree, s_cost_spans[k].lo, *hi)) &&
            (!lo || TipInKeyLess(tree, *lo, s_cost_spans[k].hi)))
            return true;
    return false;
}

/* The node of a level's run (`n`) of `tree` over a leaf whose lower key is
 * `lo` (none for the first): the last whose own is not past it. */
static uint32_t StandCostOver(uint64_t tree, const TallyKey* lo, const FoliationNode* run, uint32_t n)
{
    uint32_t at = 0;
    for (uint32_t x = 1; lo && x < n; x++)
        if (run[x].place.has_lo && !TipInKeyLess(tree, *lo, run[x].place.lo))
            at = x;
    return at;
}

/* One tree's blocks of one copy, counted here; *seen false where DogEar held
 * more spans than the count takes. */
static uint64_t StandCostTree(TagFSVolume* vol, uint64_t tree, bool* seen)
{
    uint32_t   ns = DogEarStand(vol, tree, false, s_cost_spans, STAND_COST_SPANS), nleaf = 0;
    uint32_t   above[BINDER_LEVELS_MOST];
    uint64_t   blocks = 0;
    TallyPlace p;
    *seen = *seen && ns <= STAND_COST_SPANS;
    ns    = ns < STAND_COST_SPANS ? ns : STAND_COST_SPANS;
    if (tree == TALLY_DIGESTS)
        nleaf = TallyDigestLeaves(vol);
    else
        (void)FoliationLeaves(vol, tree, &nleaf);
    if (ns == 0)
        return 0;
    if (nleaf == 0)
        return 1u;
    memset(above, 0xff, sizeof(above));
    for (uint32_t i = 0; i < nleaf && StandCostLeaf(vol, tree, i, &p); i++) {
        const TallyKey* lo = i > 0 && p.has_lo ? &p.lo : NULL;
        const TallyKey* hi = p.has_hi ? &p.hi : NULL;
        if (!StandCostMet(tree, ns, lo, hi))
            continue;
        blocks += StandCostKeeps(vol, tree, lo, hi) ? p.ptr.sheets : 0;
        for (uint64_t level = 1; level < BINDER_LEVELS_MOST; level++) {
            uint32_t             n   = 0;
            const FoliationNode* run = FoliationLevel(vol, tree, level, &n);
            if (!run || n == 0)
                break;
            uint32_t at = StandCostOver(tree, lo, run, n);
            if (above[level] != at) {
                blocks       += run[at].place.ptr.sheets;
                above[level]  = at;
            }
        }
    }
    return blocks;
}

/* What the next Tally costs by the rule, in bytes, as BinderStand says it:
 * both copies and the slot, never past the last Tally's whole book; false
 * where DogEar held more spans of a tree than the count takes. */
static bool StandCostCounted(TagFSVolume* vol, const BinderStandSaid* said, uint64_t* bytes)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    bool                  seen    = true;
    uint64_t              tip     = 0;
    for (uint32_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++)
        tip += StandCostTree(vol, trees[i], &seen);
    *bytes = (2u * (tip < said->tally_blocks ? tip : said->tally_blocks) + 1u) * TAGFS_BLOCK_SIZE;
    return seen;
}

// ----------------------------------------------------------------------------
// A leaf costs the next Tally what is left live in it (§16.6 п.6): data given
// up costs the path above its leaves, and the Tally is owed at once
// ----------------------------------------------------------------------------

#define STAND_TIP_IN_LIVES_LEAVES  12u   /* leaves of DIGESTS the data's digests begin, at the least */

/* The leaves of DIGESTS the reads stand on whose first key is a block of the
 * ground of `fid`. */
static uint32_t StandTipInLeavesOf(TagFSVolume* vol, uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(vol->state.files, fid, NULL);
    StripWalk   w;
    Strip       s;
    TallyPlace  p;
    bool        rot = false;
    uint32_t    n   = 0;
    if (!rec)
        return 0;
    StripWalkBegin(rec, &w);
    while (StripWalkNextGround(&w, &s))
        for (uint32_t i = 0; TallyDigestLeafAt(vol, i, &p, &rot); i++)
            n += !rot && p.level == 0 && p.has_lo && p.lo.k1 >= s.start && p.lo.k1 < s.start + s.count;
    FileRecordRelease(rec);
    return n;
}

TestResult TestStandTipInCostLives(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid    said;
    bool               kept    = false;
    uint64_t           fid     = 0, emptied = 0, counted = 0;
    uint64_t           blocks  = (STAND_TIP_IN_LIVES_LEAVES + 2u) * DIGEST_ITEM_MOST;
    uint32_t           leaves  = 0;
    const char*        aside   = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file(vol, "tipin-lives.bin", NULL, 0, 0, &fid), 0, "a file for data is made");
    memset(g_test_buffer, 0x5a, sizeof(g_test_buffer));
    for (uint64_t k = 0; k < blocks; k += sizeof(g_test_buffer) / TAGFS_BLOCK_SIZE) {
        TEST_ASSERT_EQ(StandWriteAt(vol, fid, k * TAGFS_BLOCK_SIZE, g_test_buffer, sizeof(g_test_buffer)),
                       (int)sizeof(g_test_buffer), "its bytes written");
        if (k % 1024u == 1020u)
            TEST_ASSERT_OK(TagFSSettle(vol), "the writes settle a batch at a time");
    }
    TEST_ASSERT_OK(TagFSSettle(vol), "the data settled");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of it is written: its DIGESTS names the data");
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounts on it, a stitch of it settled");
    BinderStand(vol, &said);
    for (uint32_t t = 0; said.demanded && t < 4u; t++) {
        TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "the Tally a DONE demanded is written");
        BinderStand(vol, &said);
    }
    TEST_ASSERT(!said.owed, "no Tally is owed: none demanded, and the ribbon past the slot is short");
    leaves  = StandTipInLeavesOf(vol, fid);
    emptied = (2u * leaves + 1u) * TAGFS_BLOCK_SIZE;
    TestEvidence("leaves of DIGESTS the data's digests begin", leaves);
    TEST_ASSERT(leaves >= STAND_TIP_IN_LIVES_LEAVES, "the data's digests take a leaf of DIGESTS a window");
    for (uint64_t k = 0; k < blocks; k += DIGEST_ITEM_MOST)
        TEST_ASSERT_EQ(StandWriteAt(vol, fid, k * TAGFS_BLOCK_SIZE, g_test_buffer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                       "a block in each window written again");
    TEST_ASSERT_OK(TagFSSettle(vol), "the writes settled");
    TEST_ASSERT_OK(StandStitchSayLong(vol, emptied - 2u * TAGFS_BLOCK_SIZE, 'l', 900u, &kept),
                   "a word two blocks short of what those leaves cost, both copies and the slot, is settled");
    BinderStand(vol, &said);
    TestEvidence("the next Tally reckoned, bytes, every leaf of the data written in", said.cost);
    TestEvidence("useful bytes past the slot's at", said.useful);
    TEST_ASSERT(said.cost >= emptied && !said.owed,
                "every leaf of the data keeps its keys: the next Tally is reckoned at them all, and is not owed");
    TEST_ASSERT(StandCostCounted(vol, &said, &counted), "every span DogEar holds is counted");
    TEST_ASSERT_EQ(said.cost, counted, "and the reckoning is the rule's, counted here (§16.6 п.6)");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, fid, INK_UNWARRANTED), 0, "the file is given up");
    TEST_ASSERT_OK(TagFSSettle(vol), "the delete settled");
    BinderStand(vol, &said);
    TestEvidence("the next Tally reckoned, bytes, the data given up", said.cost);
    TestEvidence("useful bytes past the slot's at", said.useful);
    TEST_ASSERT(said.cost < emptied, "the leaves the data emptied cost nothing: the next Tally is reckoned at the "
                "path above them and the leaves left live (§16.6 п.6)");
    TEST_ASSERT(StandCostCounted(vol, &said, &counted), "every span DogEar holds is counted");
    TestEvidence("the next Tally by the rule, counted here, bytes", counted);
    TEST_ASSERT_EQ(said.cost, counted, "the reckoning is the rule's to the block: no leaf the book's own ground or "
                   "what rests in Fallow lies in is counted live, and the path above every emptied leaf is");
    TEST_ASSERT(said.owed && said.useful < TALLY_SLACK * emptied,
                "and it is owed at once — reckoned again past the keys the delete let go — so the ribbon a mount "
                "replays is bounded by what lives, not by what was given up (§4.7)");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A leaf of FILES whose every file is given up costs nothing, the path above
// it what it did
// ----------------------------------------------------------------------------

#define STAND_COST_FILES  160u   /* files of a long name: FILES takes leaves past the volume's own */

static int StandCostCreate(TagFSVolume* vol, const char* name, uint64_t* fid)
{
    int rc = tagfs_create_file(vol, name, NULL, 0, 0, fid);
    if (rc == -(int)ERR_WOULD_BLOCK && TagFSSettle(vol) == OK)
        rc = tagfs_create_file(vol, name, NULL, 0, 0, fid);
    return rc;
}

/* A leaf of FILES past the first and before the last whose ids are all in
 * [first, last]: its place, and the ids [*from, *to) it holds keys of. */
static bool StandCostFilesLeaf(TagFSVolume* vol, uint64_t first, uint64_t last, TallyPlace* p, uint64_t* from, uint64_t* to)
{
    uint32_t n = 0;
    (void)FoliationLeaves(vol, TALLY_FILES, &n);
    for (uint32_t i = 1; i + 1u < n && FoliationLeafAt(vol, TALLY_FILES, i, p); i++) {
        *from = p->lo.k1;
        *to   = p->hi.k2 ? p->hi.k1 + 1u : p->hi.k1;
        if (p->has_lo && p->has_hi && *from >= first && *to <= last + 1u && *from < *to)
            return true;
    }
    return false;
}

TestResult TestStandTipInCostFiles(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid    said;
    TallyPlace         leaf;
    char               name[80];
    uint64_t           fid = 0, first = 0, last = 0, from = 0, to = 0, counted = 0;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    for (uint32_t i = 0; i < STAND_COST_FILES; i++) {
        ksnprintf(name, sizeof(name), "tipin-cost-%05u-of-a-name-long-enough-to-fill-leaves.txt", i);
        TEST_ASSERT_EQ(StandCostCreate(vol, name, &fid), 0, "a file of a long name is made");
        first = i == 0 ? fid : first;
        last  = fid;
    }
    TEST_ASSERT_OK(TagFSSettle(vol), "the files settled");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of them is written: FILES names them in leaves of their own");
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounts on it, a stitch of it settled");
    BinderStand(vol, &said);
    for (uint32_t t = 0; said.demanded && t < 4u; t++) {
        TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "the Tally a DONE demanded is written");
        BinderStand(vol, &said);
    }
    TEST_ASSERT(StandCostFilesLeaf(vol, first, last, &leaf, &from, &to),
                "a leaf of FILES holds keys of the test's files alone");
    TestEvidence("the files the leaf holds keys of", to - from);
    for (uint64_t f = from; f < to; f++)
        TEST_ASSERT_EQ(tagfs_delete_file(vol, f, INK_UNWARRANTED), 0, "every file of the leaf is given up");
    TEST_ASSERT_OK(TagFSSettle(vol), "the give-ups settled");
    TEST_ASSERT(!StandCostKeeps(vol, TALLY_FILES, &leaf.lo, &leaf.hi), "no key of the state is left in the leaf");
    BinderStand(vol, &said);
    TEST_ASSERT(StandCostCounted(vol, &said, &counted), "every span DogEar holds is counted");
    TestEvidence("the next Tally reckoned, bytes", said.cost);
    TestEvidence("the next Tally by the rule, counted here, bytes", counted);
    TEST_ASSERT_EQ(said.cost, counted, "the emptied leaf costs nothing and the path above it what it did; every "
                   "other leaf the give-ups meet its Sheets (§16.6 п.6)");
    for (uint64_t f = first; f <= last; f++)
        (void)tagfs_delete_file(vol, f, INK_UNWARRANTED);
    TEST_ASSERT_OK(TagFSSettle(vol), "the test's other files are given up");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The volume let go of with a Tally under way: nothing is weighed
// ----------------------------------------------------------------------------

TestResult TestStandTipInTornDown(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           fid   = 0;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(StandCostCreate(vol, "tipin-torn-down.bin", &fid), 0, "a file is made");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled: DogEar holds its key");
    BinderDemanded(vol);
    TEST_ASSERT(BinderTurn(vol) && BinderHoldsGround(vol), "a Tally is begun, its slice taken in its first turn");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is let go of between two turns of it — its file map, registry "
                "and Use Context gone before the book, nothing weighed — and mounted again");
    TEST_ASSERT(!BinderHoldsGround(vol), "no Tally is under way on the volume mounted again");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, fid, INK_UNWARRANTED), 0, "the file is given up");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
