#include "stand_stitch.h"
#include "volume_ribbon.h"
#include "volume_tally.h"
#include "../mount/away.h"
#include "../block_map/block_map_derived.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../tag_registry/tag_registry.h"
#include "../ink/ink.h"
#include "block_fail.h"
#include "boardroom.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_stitch_edge suite: a book's writer at the edges of a Quire, and
// the measure of what a whole Tally takes (spec_v2 §2.7, §3.2, §3.6, §4.5
// п.5, §16.2 п.24)
//
// A write the medium refuses is taken again only where the mount's reader
// takes a retry up — in the next_quire the last stitch sewn names, at its
// start or on the atom past an attempt refused there, never over it (§16.4
// п.4) — and nowhere else: refusals that run that Quire to its end leave the
// volume only to be read, with nothing more written. A stitch that runs on
// into its next_quire leaves two atoms of it, for a stitch that takes nothing
// and the one that gives the next. The mount's reader reads each unit of the
// chain once, a next_quire well into its stitches or not. And the reserve's
// measure of a whole Tally holds above the Tally every writer lays — tags
// longer than a Sheet, each alone in a node of two, between short ones.
//
// Every test puts the ribbon back as it found it (stand_stitch.h).
// ============================================================================

#define STAND_EDGE_PAIRS  100u    /* long tags, each with a short one after it */
#define STAND_EDGE_LONG   4000u   /* a long tag's text: longer than a Sheet's room for one item */

static char s_edge_text[STAND_EDGE_LONG + 1u];

/* The volume mounted again on what its medium holds: its ribbon ends on
 * `last`, nothing torn, no lie, and `walked` lines for what the walk of a
 * next_quire passed over to a retry (§16.4 п.4) — no other branch. */
static bool StandEdgeEndsOn(uint64_t last, uint32_t walked)
{
    MountBookSaid said;
    if (!StandStitchRemount())
        return false;
    MountBookStandSaid(&said);
    return said.last == last && !said.lied && said.ribbon[RIBBON_CODE_RIBBON_TORN] == 0 &&
           said.ribbon[RIBBON_CODE_DEAD_BRANCH] == walked && !TagFSIsUnderGlass();
}

/* The newest slot's ribbon walked by its StitchHeads alone — the walk the
 * mount reads a slot it does not stand on by (ribbon_heads.c RibbonQuiresOf),
 * and once a Tally is written over another slot the slot stood on now is one:
 * it ends on `last` and runs through `in`, the Quire a retry lies in, as the
 * mount's own walk does (§16.4 п.4, rule (б′)). */
static bool StandEdgeHeadsReach(uint64_t last, const RibbonQuire* in)
{
    SpineSlot*  slots = NULL;
    RibbonWalk* w     = NULL;
    uint32_t    count = 0;
    bool        held  = false;
    char        what[96];
    if (SpineRead(&g_book.frame, &slots, &count, what, sizeof(what)) != OK || count == 0) {
        SpineLetGo(slots, count);
        return false;
    }
    if (RibbonQuiresOf(&slots[0], &w) == OK && w->seq == last)
        for (uint32_t i = 0; i < w->nquires && !held; i++)
            held = RibbonQuireSame(&w->quires[i], in);
    RibbonWalkLetGo(w);
    SpineLetGo(slots, count);
    return held;
}

// ----------------------------------------------------------------------------
// M-6 inside the next_quire: a retry on the atom past a refused attempt, until
// the Quire runs out (§16.4 п.4)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchRetryQuireEntered(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    RibbonCursor             l, now;
    static StandStitchWaiter waiter;
    bool                     kept  = false;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    uint64_t left = s.at.quire.blocks * TAGFS_BLOCK_SECTORS - s.at.off;
    TEST_ASSERT_OK(StandStitchSayFilling(left, 'e', 1, &kept), "a word whose stitch fills the rest of its Quire is laid down");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && l.off == 0 && l.quire.blocks && !RibbonQuireSame(&l.quire, &s.at.quire),
                "the ribbon stands at the first sector of the Quire the stitch named next");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again: a waiter waits for a stitch of this mount");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.off == 0 && !RibbonQuireSame(&l.quire, &s.at.quire), "and the ribbon stands there still");
    BlockFailArmRefusedWrites(s.seat, s.base + l.quire.block * TAGFS_BLOCK_SECTORS, l.atom);
    SettleWaitLink(&waiter.w, InkedGen(), StandStitchAnswered);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle the waiter asks, refused at the Quire's first atom, settles");
    TEST_ASSERT_EQ(BlockFailSeen(), 1, "after the one refusal");
    TEST_ASSERT(waiter.answered && waiter.rc == OK, "and the waiter is answered OK");
    RibbonCursorStand(&now);
    TEST_ASSERT(RibbonQuireSame(&now.sewn.quire, &l.quire) && now.sewn.off == l.atom && now.last > l.given + 1u,
                "by a stitch of a new seq on the atom past the refused one, never over it");
    TEST_ASSERT_EQ(now.refused, 0, "and the refusals in a row are none once it settled");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeEndsOn(now.last, 1),
                "mounted again, its ribbon goes on past the Quire's first atom to the retry, the walk said");
    TEST_ASSERT(StandEdgeHeadsReach(now.last, &l.quire),
                "and walked by its StitchHeads alone, as a slot not stood on is, it takes the retry up the same");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
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
    TEST_ASSERT_OK(StandStitchSay("h", 1, 2, &kept),
                   "a stitch refused, and its retry at the next_quire's start refused, settle by the retry after");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "after two refusals");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.sewn.quire, &l.next) && now.sewn.off == l.atom,
                "laid in the next_quire on the atom past the refused retry");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeEndsOn(now.last, 1), "mounted again, its ribbon ends on it, the walk said");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A stitch whose write the medium refuses past its first sector — its head
 * lands, naming the stitch before it — and whose retry at the next_quire's
 * start is refused whole: the retry after that lies on the atom past it. A
 * slot not stood on is walked by its heads past the head that landed into the
 * next_quire, and on to the retry (§16.4 п.4, rule (б′)): nothing names the
 * head, so the next_quire is looked into, as pass 1 looks. */
static TestResult TestStandStitchRetryHeadLanded(void)
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
    TEST_ASSERT(kept && l.off && l.next.blocks && l.atom > 1u, "inside its Quire, the stitch naming the next_quire");
    BlockFailArmRefusedWrites(s.seat, s.base + l.quire.block * TAGFS_BLOCK_SECTORS + l.off + 1u, l.atom - 1u);
    TEST_ASSERT(BlockFailAddBadSectors(s.seat, s.base + l.next.block * TAGFS_BLOCK_SECTORS, l.atom),
                "the medium refuses a write past the first sector where the ribbon stands, and at the start of the "
                "next_quire");
    TEST_ASSERT_OK(StandStitchSay("h", 1, 2, &kept),
                   "a stitch whose head landed, and its retry at the next_quire's start refused, settle by the "
                   "retry after");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "after two refusals");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.sewn.quire, &l.next) && now.sewn.off == l.atom,
                "laid in the next_quire on the atom past the refused retry");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeHeadsReach(now.last, &l.next),
                "walked by its StitchHeads alone, the ribbon goes on past the head that landed into the "
                "next_quire, to the retry");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static TestResult TestStandStitchRetryRunsOut(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    RibbonCursor             l, now;
    static StandStitchWaiter waiter;
    bool                     kept  = false;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(StandStitchSay("n", 1, 1, &kept), "a word is laid down");
    TEST_ASSERT(kept && StandStitchRemount(), "the volume is mounted again: a waiter waits for a stitch of this mount");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.off && l.next.blocks, "inside its Quire, the stitch naming the next_quire");
    BlockFailArmRefusedWrites(s.seat, s.base + l.quire.block * TAGFS_BLOCK_SECTORS + l.off, l.atom);
    TEST_ASSERT(BlockFailAddBadSectors(s.seat, s.base + l.next.block * TAGFS_BLOCK_SECTORS,
                                       l.next.blocks * TAGFS_BLOCK_SECTORS),
                "the medium refuses a write where the ribbon stands, and every one in the next_quire");
    SettleWaitLink(&waiter.w, InkedGen(), StandStitchAnswered);
    TEST_ASSERT_EQ(TagFSSettle(), ERR_TAGFS_READ_ONLY,
                   "retries on atom after atom of the next_quire, refused to its end, go no further: the volume "
                   "may only be read");
    RibbonCursorStand(&now);
    /* The last atom of the next_quire takes a retry only when a Quire past it
     * was given for the retry to name: with none, the atom is kept and the
     * retry is not begun there. */
    TEST_ASSERT_EQ(now.refused, l.next.blocks * TAGFS_BLOCK_SECTORS / l.atom + (now.next.blocks ? 1u : 0u),
                   "a refusal where it stood and one on every atom of the next_quire a retry took, in a row");
    TEST_ASSERT_EQ(BlockFailSeen(), now.refused, "each a write the medium refused");
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_TAGFS_READ_ONLY, "and the waiter is told so, not answered OK");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_EQ(TagFSStandSettleForHalt(), ERR_TAGFS_READ_ONLY, "the halt's cycles are refused too");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and write nothing");
    BlockFailDisarm();
    TEST_ASSERT(StandEdgeEndsOn(l.last, 0), "mounted again, its ribbon ends on the last stitch sewn");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
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
    TEST_ASSERT_EQ(StandStitchSayFilling(room, 'j', 2, &kept), ERR_TAGFS_NO_SPACE,
                   "a word whose stitch would fill the rest of its Quire and all of the next is refused: no reader "
                   "could say where the ribbon goes on past it");
    TEST_ASSERT_EQ(StandStitchSayFilling(room - l.atom, 'k', 3, &kept), ERR_TAGFS_NO_SPACE,
                   "nor one whose stitch leaves one atom of the next: two are kept, for a stitch that takes "
                   "nothing and the one that gives its next_quire");
    RibbonCursorStand(&now);
    TEST_ASSERT(!kept && RibbonQuireSame(&now.quire, &l.quire) && now.off == l.off + 2u * l.atom,
                "each cycle laid its stitch without the word — an atom each — and the volume keeps the word it "
                "holds");
    room -= 2u * l.atom;
    TEST_ASSERT_OK(StandStitchSayFilling(room - 2u * l.atom, 'm', 4, &kept), "one that leaves two atoms is laid down");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.quire, &l.next) &&
                now.off == l.next.blocks * TAGFS_BLOCK_SECTORS - 2u * l.atom,
                "and the ribbon stands two atoms from the end of the Quire it ran on into");
    TEST_ASSERT(StandEdgeEndsOn(now.last, 0), "mounted again, its ribbon ends on that stitch");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The mount's reader reads each unit of the chain once (§16.4 п.4)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchWalkReadsOnce(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  now;
    MountBookSaid said;
    bool          kept  = false;
    uint64_t      k     = 40, word = 8u * TALLY_SHEET_PAYLOAD, found_read = 0, found_sewn = 0;
    const char*   aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    /* What the walk of the ribbon as the test found it costs: the stitches
     * the tests before sewed past the Tally — a retry among them with the
     * attempt the medium refused, a unit read and never sewn (M-6) — are
     * read by every mount after, and are no part of this test's walk. */
    uint64_t at_found = RibbonUnitsReadStand();
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on the ribbon as it was found");
    found_read = RibbonUnitsReadStand() - at_found;
    MountBookStandSaid(&said);
    found_sewn = said.sewn;
    RibbonCursorStand(&now);
    for (; RibbonQuireSame(&now.quire, &s.at.quire) && k < 40u + 4096u; k++) {
        TEST_ASSERT_OK(StandStitchSayLong(word, (char)('a' + k % 26u), k, &kept), "a long word is laid down");
        RibbonCursorStand(&now);
    }
    TEST_ASSERT(RibbonQuireSame(&now.quire, &s.at.next), "word after word, until the ribbon stands in its next_quire");
    for (uint64_t more = k + 8u; k < more; k++)
        TEST_ASSERT_OK(StandStitchSayLong(word, (char)('a' + k % 26u), k, &kept), "and eight more are laid down there");
    uint64_t before = RibbonUnitsReadStand();
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    uint64_t read = RibbonUnitsReadStand() - before;
    MountBookStandSaid(&said);
    TestEvidence("units of the chain the mount read past those it read of the ribbon as found", read - found_read);
    TEST_ASSERT(said.sewn >= found_sewn + (k - 40u), "with every stitch sewn on");
    TEST_ASSERT(read <= found_read + ((uint64_t)said.sewn - found_sewn) + 8u,
                "each unit of the chain read once: rules (б) and (б′) are weighed for no stitch a later one names, "
                "and the next_quire is not walked for every stitch of the Quire before it (§16.4 п.4)");
out:
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
    TestEvidence("blocks the reserve measures the next Tally at:", cost);
    TEST_ASSERT(cost >= laid, "the measure is never less than the Tally a writer lays of the same tags");
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

// ----------------------------------------------------------------------------
// On a removable medium the ribbon is laid in atoms of a flash page: no new
// stitch shares a page with a settled one (§16.6 п.2, C18)
// ----------------------------------------------------------------------------

/* The page the owner set the atom of a removable medium's ribbon to (§16.6
 * п.2) — stated here, not taken from the kernel it holds to. */
#define STAND_FLASH_PAGE_BYTES 16384u

/* Why a test of a removable medium's ribbon stands aside here, or NULL — its
 * page in sectors in *page. */
static const char* StandPageAside(StandStitch* s, uint32_t* page)
{
    const char* aside = StandStitchAside(s);
    if (aside)
        return aside;
    if (!BoardroomSeatIsRemovable(s->seat))
        return "the volume is on a medium that is not removable: its ribbon keeps the medium's atom";
    *page = (g_book.atom > STAND_FLASH_PAGE_BYTES ? g_book.atom : STAND_FLASH_PAGE_BYTES) / BOOK_SECTOR_BYTES;
    return NULL;
}

/* Whether `p` of a volume begins on a page of `page` sectors: a ground laid on
 * a mebibyte begins on one. */
static bool StandOnPage(const RibbonPlace* p, uint32_t page)
{
    return (p->quire.block * TAGFS_BLOCK_SECTORS + p->off) % page == 0;
}

/* Whether `q` is laid on pages of `page` sectors: its first block and its
 * length whole pages. */
static bool StandLaidOnPage(const RibbonQuire* q, uint32_t page)
{
    uint64_t pb = page / TAGFS_BLOCK_SECTORS;
    return q->blocks && q->block % pb == 0 && q->blocks % pb == 0;
}

/* A Quire of the ribbon's size begun a block past a page, on ground free past
 * the ribbon's Quire, as a writer of a smaller atom takes a next_quire: its
 * run taken from the map of the volume mounted, the claim let go with it. */
static bool StandOffPageQuire(const StandStitch* s, uint32_t page, RibbonQuire* out)
{
    uint64_t start = 0, pb = page / TAGFS_BLOCK_SECTORS;
    if (BlockMapTakeQuire(GROUND_RIBBON, s->at.quire_blocks + pb, (uint32_t)pb, 0, g_book.frame.data_block,
                          s->at.quire.block + s->at.quire.blocks, &start) != OK)
        return false;
    *out = (RibbonQuire){ start + 1u, s->at.quire_blocks };
    return true;
}

/* Why a test that lays a stitch of `of` sectors at the ribbon's end naming a
 * next_quire begun off a page (*off) stands aside here, or NULL. */
static const char* StandOffPageAside(StandStitch* s, uint32_t* page, uint32_t of, RibbonQuire* off)
{
    const char* aside = StandPageAside(s, page);
    if (aside)
        return aside;
    if (s->at.off + of + 2u * *page > s->at.quire.blocks * TAGFS_BLOCK_SECTORS || !StandOffPageQuire(s, *page, off))
        return "the ribbon has no room left in its Quire for three stitches, or no Quire is free past it";
    return NULL;
}

static TestResult TestStandStitchPageAtom(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor l;
    uint32_t     page  = 0;
    bool         kept  = false;
    const char*  aside = StandPageAside(&s, &page);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(s.at.atom, page, "on a removable medium the ribbon is written in atoms of a flash page");
    TEST_ASSERT_OK(StandStitchSay("page,atom", 9u, 1, &kept), "a word is said");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && StandOnPage(&l.sewn, page) && l.sewn_of % page == 0,
                "its stitch begins on a page and is whole pages long");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.atom == page && l.off % page == 0 && l.quire.block % (page / TAGFS_BLOCK_SECTORS) == 0,
                "and its ribbon goes on on a page, in pages");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/*
 * A ribbon whose last stitch ends inside a page — laid by a mount of a smaller
 * atom, as a fixed medium's is — goes on at the start of the next_quire that
 * stitch names, the one place past it a reader takes a stitch up (rule (б)),
 * never in that page; and the walk of the next mount takes the stitch there up.
 */
static TestResult TestStandStitchPageAtomHop(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  l, now;
    MountBookSaid said;
    uint32_t      page  = 0;
    bool          kept  = false;
    const char*   aside = StandPageAside(&s, &page);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(s.at.atom, page, "on a removable medium the ribbon is written in atoms of a flash page");
    if (!s.at.next.blocks || s.at.off + page > s.at.quire.blocks * TAGFS_BLOCK_SECTORS) {
        verdict = test_steps_aside("the ribbon names no next_quire, or has no page of its Quire left");
        goto out;
    }
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandStitchLayEmpty(&s, STITCH_DATA_FLUSHED) != 0,
                "a stitch a block long, as a smaller atom lays it, is laid at the ribbon's end: it ends inside a page");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on it");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.last == s.at.given + 1u && l.off == 0 && RibbonQuireSame(&l.quire, &s.at.next) &&
                RibbonQuireSame(&l.came, &s.at.quire),
                "its ribbon goes on at the start of the next_quire that stitch names, never in its page");
    TEST_ASSERT_OK(StandStitchSay("page,hop", 8u, 1, &kept), "a word is said");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.sewn.quire, &s.at.next) && now.sewn.off == 0,
                "its stitch is laid there");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.last == now.last && said.ribbon[RIBBON_CODE_DEAD_BRANCH] == 0,
                "its walk takes that stitch up from the next_quire's start, as rule (б) does: nothing of a dead "
                "branch said");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/*
 * A ribbon whose end lies on a page, its last stitch naming a next_quire a
 * writer of a smaller atom took a block past a page, goes on at its end and
 * names a next_quire laid on a page in its stead: the ribbon never goes on in
 * the one begun off a page while another can be taken, and no stitch it lays
 * shares a page with a settled one, in its Quire or the next.
 */
static TestResult TestStandStitchPageNamed(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  l, now;
    RibbonQuire   off;
    MountBookSaid said;
    uint32_t      page  = 0;
    bool          kept  = false;
    const char*   aside = StandOffPageAside(&s, &page, STAND_STITCH_LAID_MOST, &off);

    if (aside)
        return test_steps_aside(aside);
    if (page > STAND_STITCH_LAID_MOST) {
        verdict = test_steps_aside("the medium's page is longer than a stitch the stand lays by hand");
        goto out;
    }
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandStitchLayAt(&s, STITCH_DATA_FLUSHED, page, &off) != 0,
                "a stitch a page long is laid at the ribbon's end, naming a next_quire begun a block past a page");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on it");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.last == s.at.given + 1u && RibbonQuireSame(&l.quire, &s.at.quire) &&
                l.off == s.at.off + page && RibbonQuireSame(&l.named, &off),
                "its ribbon goes on at its end, on a page, past the stitch naming the next_quire begun off one");
    TEST_ASSERT_OK(StandStitchSay("page,named", 10u, 1, &kept), "a word is said");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && StandOnPage(&now.sewn, page) && StandOnPage(&(RibbonPlace){ now.quire, now.off }, page),
                "its stitch begins and ends on a page");
    TEST_ASSERT(StandLaidOnPage(&now.named, page) && !RibbonQuireSame(&now.named, &off),
                "and names a next_quire laid on a page in the stead of the one begun off a page");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.last == now.last && said.ribbon[RIBBON_CODE_DEAD_BRANCH] == 0 &&
                said.ribbon[RIBBON_CODE_RIBBON_TORN] == 0 && !TagFSIsUnderGlass(),
                "its walk takes that stitch up, and the next_quire it names");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/*
 * A ribbon whose last stitch ends inside a page, naming a next_quire begun off
 * a page — both a writer of a smaller atom's — goes on at its end, in atoms of
 * a page: the stitch it lays first shares that page with the last one settled
 * and ends on a page, naming a next_quire laid on one, and every stitch after
 * it begins on a page and is whole pages — never a mount of stitches a block
 * long (§16.2 п.11, §16.6 п.2).
 */
static TestResult TestStandStitchPageInside(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  l, now;
    RibbonQuire   off;
    MountBookSaid said;
    uint32_t      page  = 0;
    bool          kept  = false;
    const char*   aside = StandOffPageAside(&s, &page, VOLUME_BLOCK_SECTORS, &off);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandStitchLayAt(&s, STITCH_DATA_FLUSHED, VOLUME_BLOCK_SECTORS, &off) != 0,
                "a stitch a block long is laid at the ribbon's end — it ends inside a page — naming a next_quire "
                "begun a block past a page");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on it");
    RibbonCursorStand(&l);
    TEST_ASSERT(l.last == s.at.given + 1u && RibbonQuireSame(&l.quire, &s.at.quire) &&
                l.off == s.at.off + VOLUME_BLOCK_SECTORS && l.atom == page,
                "its ribbon goes on at its end, inside the page, written in atoms of a page");
    TEST_ASSERT_OK(StandStitchSay("page,inside", 11u, 1, &kept), "a word is said");
    RibbonCursorStand(&now);
    TEST_ASSERT(kept && RibbonQuireSame(&now.sewn.quire, &l.quire) && now.sewn.off == l.off &&
                StandOnPage(&(RibbonPlace){ now.quire, now.off }, page),
                "its stitch begins where the last one ended and ends on a page");
    TEST_ASSERT(StandLaidOnPage(&now.named, page), "naming a next_quire laid on a page");
    TEST_ASSERT_OK(StandStitchSay("page,after", 10u, 2, &kept), "a second word is said");
    RibbonCursorStand(&l);
    TEST_ASSERT(kept && StandOnPage(&l.sewn, page) && l.sewn_of % page == 0,
                "the stitch after it begins on a page and is whole pages");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.last == l.last && said.ribbon[RIBBON_CODE_DEAD_BRANCH] == 0 &&
                said.ribbon[RIBBON_CODE_RIBBON_TORN] == 0 && !TagFSIsUnderGlass(),
                "its walk takes both stitches up");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_stitch_edge_tests[] = {
    { .name = "stand_stitch_retry_quire_entered", .func = TestStandStitchRetryQuireEntered,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_refused_twice", .func = TestStandStitchRetryRefusedTwice,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_head_landed", .func = TestStandStitchRetryHeadLanded,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_runs_out", .func = TestStandStitchRetryRunsOut,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_runs_on_leaves", .func = TestStandStitchRunsOnLeaves,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_walk_reads_once", .func = TestStandStitchWalkReadsOnce,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_tally_cost_long", .func = TestStandStitchTallyCostLong,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_page_atom", .func = TestStandStitchPageAtom,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_page_atom_hop", .func = TestStandStitchPageAtomHop,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_page_named", .func = TestStandStitchPageNamed,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_page_inside", .func = TestStandStitchPageInside,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_registry_counts_from_nothing", .func = TestStandRegistryCountsFromNothing,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_stitch_edge_suite = {
    g_stand_stitch_edge_tests, sizeof(g_stand_stitch_edge_tests) / sizeof(g_stand_stitch_edge_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
