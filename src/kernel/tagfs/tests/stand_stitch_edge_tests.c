#include "stand_stitch.h"
#include "volume_ribbon.h"
#include "volume_tally.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../tag_registry/tag_registry.h"
#include "../file_map/file_map.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_stitch_edge suite: a book's writer at the edges of a Quire, and
// the measure of what a whole Tally takes (spec_v2 §2.7, §3.2, §3.6, §4.5
// п.5, §16.2 п.24)
//
// A write the medium refuses is taken again only where the mount's reader
// takes a retry up — at the start of the next_quire the last stitch sewn
// names, past the Quire it ends in — and nowhere else: a refusal of the first
// stitch of a Quire the ribbon entered, or of a retry, leaves the volume only
// to be read, with nothing more written. A stitch that runs on into its
// next_quire leaves two atoms of it, for a stitch that takes nothing and the
// one that gives the next. And the reserve's measure of a whole Tally holds
// above the Tally every writer lays — tags longer than a Sheet, each alone in
// a node of two, between short ones.
//
// Every test puts the ribbon back as it found it (stand_stitch.h).
// ============================================================================

#define STAND_EDGE_SLACK  200u    /* bytes a stitch's head, its UseContext's and its Clasp take, and more */
#define STAND_EDGE_PAIRS  100u    /* long tags, each with a short one after it */
#define STAND_EDGE_LONG   4000u   /* a long tag's text: longer than a Sheet's room for one item */

static char s_edge_text[STAND_EDGE_LONG + 1u];

/* A word whose stitch takes exactly `sectors` sectors — a whole number of
 * atoms — said to the volume as its `k`-th: a stitch's stream fills
 * BOOK_PAYLOAD_BYTES a sector, and its head, its UseContext's tag and length
 * and its Clasp take fewer than STAND_EDGE_SLACK bytes and more than none. */
static error_t StandEdgeSayFilling(uint64_t sectors, char letter, uint64_t k, bool* kept)
{
    return StandStitchSayLong(sectors * BOOK_PAYLOAD_BYTES - STAND_EDGE_SLACK, letter, k, kept);
}

/* The volume mounted again on what its medium holds: its ribbon ends on
 * `last`, nothing torn, no branch, no lie. */
static bool StandEdgeEndsOn(uint64_t last)
{
    MountBookSaid said;
    if (!StandStitchRemount())
        return false;
    MountBookStandSaid(&said);
    return said.last == last && !said.lied && said.ribbon[RIBBON_CODE_RIBBON_TORN] == 0 &&
           said.ribbon[RIBBON_CODE_DEAD_BRANCH] == 0 && !TagFSIsUnderGlass();
}

// ----------------------------------------------------------------------------
// M-6 where no reader takes a retry up: the volume is only read (§4.5 п.5)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchRetryQuireEntered(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor l;
    bool         kept  = false;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    uint64_t left = s.at.quire.blocks * TAGFS_BLOCK_SECTORS - s.at.off;
    TEST_ASSERT_OK(StandEdgeSayFilling(left, 'e', 1, &kept), "a word whose stitch fills the rest of its Quire is laid down");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && l.off == 0 && l.quire.blocks && !RibbonQuireSame(&l.quire, &s.at.quire),
                "the ribbon stands at the first sector of the Quire the stitch named next");
    BlockFailArmRefusedWrites(s.seat, s.base + l.quire.block * TAGFS_BLOCK_SECTORS, l.atom);
    TEST_ASSERT_EQ(StandStitchSay("f", 1, 2, &kept), ERR_TAGFS_READ_ONLY,
                   "the next stitch, refused there, is taken again nowhere: the last stitch sewn names no Quire "
                   "past the one it begins in, where a reader takes a retry up — the volume may only be read");
    TEST_ASSERT_EQ(BlockFailSeen(), 1, "after the one refusal");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_EQ(TagFSSettle(), ERR_TAGFS_READ_ONLY, "a cycle asked after it is refused");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and nothing more is written");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeEndsOn(l.last), "mounted again, its ribbon ends on the stitch that filled the Quire");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static TestResult TestStandStitchRetryRefusedTwice(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor l, now;
    bool         kept  = false;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(StandStitchSay("g", 1, 1, &kept), "a word is laid down");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && l.off && l.next.blocks, "inside its Quire, the stitch naming the next_quire");
    BlockFailArmRefusedWrites(s.seat, s.base + l.quire.block * TAGFS_BLOCK_SECTORS + l.off, l.atom);
    TEST_ASSERT(BlockFailAddBadSectors(s.seat, s.base + l.next.block * TAGFS_BLOCK_SECTORS, l.atom),
                "the medium refuses a write where the ribbon stands, and at the start of the next_quire");
    TEST_ASSERT_EQ(StandStitchSay("h", 1, 2, &kept), ERR_TAGFS_READ_ONLY,
                   "a stitch refused, and its retry at the next_quire refused, go no further: the volume may only "
                   "be read");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "after two refusals");
    RibbonCursorStand(&now);
    TEST_ASSERT_EQ(now.refused, 2, "counted in a row");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_EQ(TagFSStandSettleForHalt(), ERR_TAGFS_READ_ONLY, "the halt's cycles are refused too");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and write nothing");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeEndsOn(l.last), "mounted again, its ribbon ends on the last stitch sewn");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A stitch that runs on into its next_quire leaves two atoms of it (§3.6)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchRunsOnLeaves(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor l, now;
    bool         kept  = false;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(StandStitchSay("i", 1, 1, &kept), "a word is laid down");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && l.next.blocks, "the ribbon names its next_quire");
    uint64_t room = l.quire.blocks * TAGFS_BLOCK_SECTORS - l.off + l.next.blocks * TAGFS_BLOCK_SECTORS;
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_EQ(StandEdgeSayFilling(room, 'j', 2, &kept), ERR_TAGFS_NO_SPACE,
                   "a stitch that would fill the rest of its Quire and all of the next is not begun: no reader "
                   "could say where the ribbon goes on past it");
    TEST_ASSERT_EQ(StandEdgeSayFilling(room - l.atom, 'k', 3, &kept), ERR_TAGFS_NO_SPACE,
                   "nor one that leaves one atom of the next: two are kept, for a stitch that takes nothing and "
                   "the one that gives its next_quire");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "with not one write");
    BlockFailDisarm();
    TEST_ASSERT_OK(StandEdgeSayFilling(room - 2u * l.atom, 'm', 4, &kept), "one that leaves two atoms is laid down");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.quire, &l.next) &&
                now.off == l.next.blocks * TAGFS_BLOCK_SECTORS - 2u * l.atom,
                "and the ribbon stands two atoms from the end of the Quire it ran on into");
    TEST_ASSERT(StandEdgeEndsOn(now.last), "mounted again, its ribbon ends on that stitch");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The reserve's measure holds above the Tally a writer lays (§16.2 пп.6, 24)
// ----------------------------------------------------------------------------

/* The Sheets of the TAGS tree's leaves as every writer of a Tally lays them
 * (§16.2 п.24, volume-read's binder.Pack): the registry's tags in id order,
 * each leaf filled greedily, a tag longer than a Sheet alone in a node of as
 * many Sheets as it takes. */
static uint64_t StandEdgeTagLeaves(const TagRegistry* reg)
{
    uint64_t sheets = 0, size = 0, n = 0;
    TallyKey first = { 0, 0 }, prev = { 0, 0 };
    for (uint32_t id = 0; id < reg->next_id; id++) {
        TagSpelling sp;
        if (!TagRegistrySpelling((TagRegistry*)reg, id, &sp, NULL))
            continue;
        uint32_t body = TallyVarintBytes(sp.value ? TALLY_TAG_HAS_VALUE : 0u) + TallyVarintBytes(sp.key_len) +
                        sp.key_len + (sp.value ? TallyVarintBytes(sp.value_len) + sp.value_len : 0u);
        TallyKey key  = { id, 0 };
        if (n) {
            NodeHead was = { TALLY_TAGS, 0, n, first }, grown = { TALLY_TAGS, 0, n + 1u, first };
            uint64_t to  = size + TallyItemBytes(TALLY_TAGS, key, prev, body) + NodeHeadBytes(&grown) -
                           NodeHeadBytes(&was);
            if (to <= TALLY_SHEET_PAYLOAD) {
                size = to;
                prev = key;
                n++;
                continue;
            }
            sheets += (size + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD;
        }
        NodeHead one = { TALLY_TAGS, 0, 1, key };
        size  = NodeHeadBytes(&one) + TallyItemBytes(TALLY_TAGS, key, key, body);
        first = prev = key;
        n     = 1;
        if (size > TALLY_SHEET_PAYLOAD) {
            sheets += (size + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD;
            n = 0;
        }
    }
    return sheets + (n ? (size + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD : 0u);
}

/* The blocks of the Tally the mount stands on, but its TAGS tree's. */
static uint64_t StandEdgeTallyButTags(void)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_USE, TALLY_DIGESTS };
    uint64_t              blocks  = 0;
    TallyPlace            at;
    for (uint32_t t = 0; t < ARRAY_SIZE(trees); t++)
        for (uint32_t i = 0; TallyStandNodeWhere(trees[t], i, &at); i++)
            blocks += at.ptr.sheets;
    return blocks;
}

/* A tag of `len` bytes of text given an id: "stitch:<letter><n>-xxx…". */
static bool StandEdgeTag(TagRegistry* reg, char letter, uint32_t n, uint32_t len)
{
    TagSpelling tag;
    uint32_t    tid = 0;
    uint32_t    at  = (uint32_t)ksnprintf(s_edge_text, sizeof(s_edge_text), "stitch:%c%05u-", letter, n);
    memset(s_edge_text + at, 'x', len > at ? len - at : 0u);
    s_edge_text[len > at ? len : at] = '\0';
    return TagSpell(s_edge_text, len > at ? len : at, &tag) == OK && TagRegistryIntern(reg, &tag, &tid) == OK;
}

static TestResult TestStandStitchTallyCostLong(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    TagRegistry* reg   = tagfs_get_state()->registry;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    for (uint32_t i = 0; i < STAND_EDGE_PAIRS; i++)
        TEST_ASSERT(StandEdgeTag(reg, 'l', i, STAND_EDGE_LONG) && StandEdgeTag(reg, 's', i, 0),
                    "tags longer than a Sheet's room are given ids, each with a short one after it");
    uint64_t laid = StandEdgeTallyButTags() + StandEdgeTagLeaves(reg);
    uint64_t cost = TallyCostBlocks();
    TestEvidence("blocks a writer lays of the Tally, the TAGS tree's leaves alone:", laid);
    TestEvidence("blocks the reserve measures a whole Tally at:", cost);
    TEST_ASSERT(cost >= laid, "the measure is never less than the Tally a writer lays of the same tags");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A change to a file a book refuses interns no name: no Coined for it (§2.8)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchRefusedNamesNothing(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    TagRegistry* reg = tagfs_get_state()->registry;
    TagSpelling  tag;
    uint64_t     fid = 0, made = 0;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    uint32_t next = reg->next_id;
    TEST_ASSERT(FileMapFiles(tagfs_get_state()->files, 0, &fid, 1) == 1, "the book holds a file");
    TEST_ASSERT_EQ(tagfs_create_file("edge-refused.bin", NULL, 0, &made), -(int)ERR_TAGFS_READ_ONLY,
                   "a file is not made on a book");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "edge-renamed.bin", INK_WARRANTED), -(int)ERR_TAGFS_READ_ONLY,
                   "nor named anew");
    TEST_ASSERT(TagSpell("edge:refused", 12u, &tag) == OK, "a tag is spelled");
    TEST_ASSERT_EQ(tagfs_add_tag_string(fid, &tag, INK_WARRANTED), ERR_TAGFS_READ_ONLY, "nor tagged");
    TEST_ASSERT_EQ(reg->next_id, next, "and not one of the names and tags those changes carried is given an id");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The registry counts its text from nothing: the measure is built on it
// ----------------------------------------------------------------------------

static TestResult TestStandRegistryCountsFromNothing(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = kmalloc(sizeof(TagRegistry));
    bool         built   = false;
    TagSpelling  tag;
    uint32_t     tid = 0, lo = 1, hi = 0;
    uint64_t     tags = 1, text = 1;

    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    memset(scratch, 0xA5, sizeof(*scratch));
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built on memory nobody cleared");
    built = true;
    TagRegistryTextBytes(scratch, &tags, &text);
    TagRegistryCoinedRange(scratch, &lo, &hi);
    TEST_ASSERT(tags == 0 && text == 0 && lo == 0 && hi == 0,
                "it holds no tag, no byte of text, and no id it gave that a book has not laid down");
    TEST_ASSERT(TagSpell("stitch:edge", 11u, &tag) == OK && TagRegistryIntern(scratch, &tag, &tid) == OK,
                "a tag is given an id");
    TagRegistryTextBytes(scratch, &tags, &text);
    TagRegistryCoinedRange(scratch, &lo, &hi);
    TEST_ASSERT(tags == 1 && text == 10u && lo == 0 && hi == tid + 1u,
                "and it holds that tag's ten bytes of text, the one id not yet laid down");
out:
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    return verdict;
}

static const TestCase g_stand_stitch_edge_tests[] = {
    { .name = "stand_stitch_retry_quire_entered", .func = TestStandStitchRetryQuireEntered,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_refused_twice", .func = TestStandStitchRetryRefusedTwice,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_runs_on_leaves", .func = TestStandStitchRunsOnLeaves,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_tally_cost_long", .func = TestStandStitchTallyCostLong,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_refused_names_nothing", .func = TestStandStitchRefusedNamesNothing,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_registry_counts_from_nothing", .func = TestStandRegistryCountsFromNothing,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_stitch_edge_suite = {
    g_stand_stitch_edge_tests, sizeof(g_stand_stitch_edge_tests) / sizeof(g_stand_stitch_edge_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
