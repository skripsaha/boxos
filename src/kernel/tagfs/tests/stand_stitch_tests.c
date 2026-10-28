#include "stand_stitch.h"
#include "stand_ribbon.h"
#include "volume_ribbon.h"
#include "../tagfs.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../block_map/block_map_derived.h"
#include "../ink/ink.h"
#include "use_context.h"
#include "amp.h"
#include "boardroom.h"
#include "block_fail.h"
#include "crypto.h"
#include "kheap.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_stitch suite: the kernel sews its own stitches on a format-v2
// volume's ribbon (spec_v2 §2.7, §3.2, §3.5, §3.6)
//
// A cycle of a book lays one stitch past the last one sewn: right after a
// mount a waiter is answered only once a stitch of this mount settled under
// its flush (X2-3); a tag given an id is laid down as a Coined and is the same
// id at the next mount; a Use Context the person changes is laid down once,
// and the same word again lays nothing; the halt's last stitch says CLEAN; a
// write the medium refuses is taken again with a new seq at the start of the
// next_quire (C25, M-6), and where no Quire is left to go on in the volume
// may only be read, its waiters told so; the ribbon does not eat the ground a
// Tally needs, and says so when it has run out; a medium shown to lie is said
// to have lied by every stitch after; and a medium that leaves and comes back
// is read again, what no stitch settled lost and its waiters told so.
//
// Every test that sews puts the ribbon back as it found it — the sectors past
// where it stood to zeros, as the maker laid them, and every Quire it went on
// into — and mounts the volume again on it, so the next test and the boot's
// last mount meet the volume the boot met. What a test lays by hand it lays by
// the Boardroom's doors, as the stand_ribbon suite does.
// ============================================================================

#define STAND_STITCH_ZEROS  64u    /* sectors of zeros a put-back writes at once */

static uint8_t s_stitch_zeros[STAND_STITCH_ZEROS * TAGFS_SECTOR_SIZE];
static uint8_t s_stitch_stream[TALLY_SHEET_PAYLOAD];
static uint8_t s_stitch_sectors[VOLUME_BLOCK_BYTES];

void StandStitchAnswered(SettleWaiter* w, error_t rc)
{
    StandStitchWaiter* s = (StandStitchWaiter*)w;
    s->rc       = rc;
    s->answered = true;
}

/* Why a test written for a book's writer stands aside here, or NULL. */
const char* StandStitchAside(StandStitch* s)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V2)
        return "the volume is not format v2: there is no ribbon to sew";
    const char* aside = StandStepsAsideFor();
    if (aside)
        return aside;
    memset(s, 0, sizeof(*s));
    RibbonCursorStand(&s->at);
    s->seat = tagfs_get_seat();
    s->base = tagfs_get_volume_base();
    memcpy(s->uuid, tagfs_get_state()->uuid, sizeof(s->uuid));
    UseList* held = UseContextHold();
    s->context = held != NULL;
    UseListDrop(held);
    if (!s->at.up)
        return "the writer has no cursor on the ribbon";
    kprintf("[TESTS] stand_stitch: the ribbon is sewn past where it stood — the mounts on it are this test's, "
            "until it is put back\n");
    return NULL;
}

/* The volume let go of — writing nothing — and mounted again on what its
 * medium holds: true when it came up. */
bool StandStitchRemount(void)
{
    if (tagfs_get_state()->initialized && !TagFSStandLetGo())
        return false;
    return StandRibbonUp();
}

/* `blocks` blocks from `block` to zeros, past TagFS. */
static bool StandStitchZero(const StandStitch* s, uint64_t block, uint64_t blocks)
{
    uint64_t vlba = block * TAGFS_BLOCK_SECTORS, end = (block + blocks) * TAGFS_BLOCK_SECTORS;
    bool     ok   = true;
    while (ok && vlba < end) {
        uint32_t n = end - vlba < STAND_STITCH_ZEROS ? (uint32_t)(end - vlba) : STAND_STITCH_ZEROS;
        ok   = BoardroomWrite(s->seat, s->base + vlba, n, s_stitch_zeros, NULL) == 0;
        vlba += n;
    }
    return ok;
}

/* A Quire the ribbon went on into, zeroed whole: none, or the one it stood in. */
static bool StandStitchZeroQuire(const StandStitch* s, const RibbonQuire* q)
{
    if (!q->blocks || RibbonQuireSame(q, &s->at.quire))
        return true;
    return StandStitchZero(s, q->block, q->blocks);
}

/*
 * The ribbon put back as the test found it: the volume let go of with no
 * caller left inside it (StandRibbonLetGo), every sector past where the ribbon
 * stood in its Quire to zeros, and every Quire it went on into since — the
 * next_quire the test found, and the Quire and the next_quire it stands at now
 * — whole; the machine holding no Use Context again when it held none, so the
 * mount does not tell the volume one it took up from a test's stitch
 * (UseContextRecall); the volume mounted again. False when a sector would not
 * take its zeros, or the volume did not come up where the ribbon stood before;
 * false, and no zeros laid, when a caller is still inside the volume at the
 * bound — a cycle of its writer would sew past them.
 */
bool StandStitchPutBack(const StandStitch* s)
{
    RibbonCursor now;
    bool         down = StandRibbonLetGo(&now), ok = down;
    if (down) {
        uint64_t first = s->at.quire.block + s->at.off / TAGFS_BLOCK_SECTORS;
        ok  = StandStitchZero(s, first, s->at.quire.block + s->at.quire.blocks - first);
        ok &= StandStitchZeroQuire(s, &s->at.next);
        ok &= StandStitchZeroQuire(s, &now.quire);
        ok &= StandStitchZeroQuire(s, &now.next);
        ok &= StandStitchZeroQuire(s, &now.sewn_hop);
    }
    if (!s->context)
        UseContextClear();
    kprintf("[TESTS] stand_stitch: the ribbon is %s\n",
            down ? "put back as it was found" : "NOT put back — a caller stayed inside the volume");
    StandRibbonArrive();
    TagFSStandHoldAgain();
    RibbonCursorStand(&now);
    bool back = ok && tagfs_get_state()->initialized && now.up && now.last == s->at.last &&
                RibbonQuireSame(&now.quire, &s->at.quire) && now.off == s->at.off;
    if (!back)
        kprintf("[TESTS] stand_stitch: put back — zeros %s, mounted %d, the ribbon at seq %llu, sector %llu of "
                "Quire %llu; it stood at seq %llu, sector %llu of Quire %llu\n",
                !down ? "not laid" : ok ? "laid" : "refused",
                (int)tagfs_get_state()->initialized, (unsigned long long)now.last, (unsigned long long)now.off,
                (unsigned long long)now.quire.block, (unsigned long long)s->at.last,
                (unsigned long long)s->at.off, (unsigned long long)s->at.quire.block);
    return back;
}

/* The last stitch this mount sewed, read back off the medium with the
 * mount's own reader: *out the caller's to let go. */
bool StandStitchRead(RibbonUnit** out)
{
    RibbonCursor c;
    RibbonCursorStand(&c);
    *out = NULL;
    if (!c.sewn_of)
        return false;
    return RibbonUnitRead(&c.sewn, c.sewn_of, c.last, &c.sewn_hop, out) == OK && *out && (*out)->sound;
}

/* Whether stitch `u` carries a record of `kind` whose body is `body`. */
static bool StandStitchCarries(const RibbonUnit* u, uint64_t kind, const uint8_t* body, uint32_t len)
{
    for (uint32_t i = 0; i < u->nrecords; i++)
        if (u->records[i].kind == kind && u->records[i].len == len &&
            memcmp(u->stream + u->records[i].body, body, len) == 0)
            return true;
    return false;
}

/* The Use Context told to the volume as the machine's `k`-th word past any it
 * said, settled by the cycle the word demands: its answer, and whether the
 * volume now holds it. */
error_t StandStitchSay(const char* text, size_t bytes, uint64_t k, bool* kept)
{
    uint64_t saying = 0;
    UseTextDrop(tagfs_recall_use_context(&saying));
    UseText* t = bytes ? UseTextMake(text, bytes) : NULL;
    if (bytes && !t)
        return ERR_NO_MEMORY;
    error_t rc = tagfs_remember_use_context(t, saying + 1000u + k, kept);
    UseTextDrop(t);
    return rc;
}

// ----------------------------------------------------------------------------
// X2-3: a waiter right after a mount is answered by a stitch of this mount
// ----------------------------------------------------------------------------

static TestResult TestStandStitchAnswersAfterFlush(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    RibbonCursor             now;
    MountBookSaid            said;
    static StandStitchWaiter waiter;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh");
    uint64_t gen = InkedGen();
    TEST_ASSERT_EQ(SettleWaitNow(gen), ERR_WOULD_BLOCK,
                   "right after the mount a wait is not answered by what the mount read (X2-3)");
    SettleWaitLink(&waiter.w, gen, StandStitchAnswered);
    SettleServiceIfPending();
    TEST_ASSERT(!waiter.answered, "the cadence held, nobody answers the waiter");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles");
    TEST_ASSERT(waiter.answered && waiter.rc == OK, "its DONE answers the waiter, OK");
    RibbonCursorStand(&now);
    TEST_ASSERT_EQ(now.last, s.at.given + 1u, "with a stitch of the seq past every one the book showed");
    TEST_ASSERT(now.sewn_flags & STITCH_DATA_FLUSHED, "which says DATA_FLUSHED: it names no fresh data");
    TEST_ASSERT(BlockFailSeen() >= 1 && BlockFailSeen() <= now.sewn_of / now.atom,
                "laid in whole atoms, in as many writes as the seat takes them in");
    BlockFailDisarm();
    TEST_ASSERT_OK(SettleWaitNow(gen), "and a wait for it now is answered at once");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.last, now.last, "its ribbon ends on the stitch the cycle sewed");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_RIBBON_TORN] + said.ribbon[RIBBON_CODE_DEAD_BRANCH], 0,
                   "with nothing torn and no branch said");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The Use Context: laid down once, the same word lays nothing (§7)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchUseContext(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonUnit*   u = NULL;
    bool          kept = false;
    const char*   aside = StandStitchAside(&s);
    static const char said[] = "stitch,sewn";
    uint8_t       body[32];

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(StandStitchSay(said, sizeof(said) - 1u, 1, &kept), "a Use Context is said to the volume");
    TEST_ASSERT(kept, "and the volume holds it");
    TEST_ASSERT(StandStitchRead(&u), "the stitch its cycle sewed reads back whole");
    uint32_t len = TallyUsePut(body, (const uint8_t*)said, sizeof(said) - 1u);
    TEST_ASSERT(StandStitchCarries(u, RIBBON_USE_CONTEXT, body, len), "and carries the word whole, as UseContext");
    RibbonUnitLetGo(u);
    u = NULL;

    uint64_t cycles = SettledCycles();
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_OK(StandStitchSay(said, sizeof(said) - 1u, 2, &kept), "the same word is said again");
    TEST_ASSERT(kept, "and the volume holds it");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "with not one write: what the book holds is not written again");
    TEST_ASSERT_EQ(SettledCycles(), cycles, "and no cycle asked for it");
    BlockFailDisarm();

    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    uint64_t saying = 0;
    UseText* back   = tagfs_recall_use_context(&saying);
    bool     same   = back && back->bytes == sizeof(said) - 1u && memcmp(back->text, said, back->bytes) == 0;
    UseTextDrop(back);
    TEST_ASSERT(same, "and recalls the word the ribbon's last UseContext says, over the Tally's");
out:
    RibbonUnitLetGo(u);
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// Coined: a tag given an id is that id at the next mount (§2.8, §6)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchCoined(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    TagSpelling  tag;
    TagRegistry* reg = tagfs_get_state()->registry;
    RibbonUnit*  u   = NULL;
    uint32_t     tid = TAGFS_INVALID_TAG_ID;
    uint8_t      body[64];
    const char*  aside = StandStitchAside(&s);
    static const char text[] = "stitch:coined";

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(TagSpell(text, sizeof(text) - 1u, &tag), "a tag is spelled");
    TEST_ASSERT_EQ(TagRegistryLookup(reg, &tag), TAGFS_INVALID_TAG_ID, "the volume does not know it");
    TEST_ASSERT_OK(TagRegistryIntern(reg, &tag, &tid), "it is given an id");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_OK(TagRegistryFlushFor(reg, tid), "the id is the registry's at once");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "with not one write: a book's registry is laid down by the cycle");
    BlockFailDisarm();
    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles");
    TEST_ASSERT(StandStitchRead(&u), "its stitch reads back whole");
    TallyTag t = { .flags = TALLY_TAG_HAS_VALUE, .key = (const uint8_t*)"stitch", .key_len = 6,
                   .value = (const uint8_t*)"coined", .value_len = 6 };
    uint32_t len = RibbonCoinedBodyPut(body, tid, &t);
    TEST_ASSERT(StandStitchCarries(u, RIBBON_COINED, body, len), "and carries the tag as Coined, its id and its text");
    RibbonUnitLetGo(u);
    u = NULL;
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    reg = tagfs_get_state()->registry;
    TEST_ASSERT_EQ(TagRegistryLookup(reg, &tag), tid, "and knows the tag by the id it was given");
out:
    RibbonUnitLetGo(u);
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The halt's last stitch says CLEAN (§16.2 п.9)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchHaltClean(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  now;
    MountBookSaid said;
    bool          shut = false;
    const char*   aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TagFSStandShutForHalt(true);
    shut = true;
    TEST_ASSERT_OK(TagFSStandSettleForHalt(), "the halt's cycles settle");
    TagFSStandShutForHalt(false);
    shut = false;
    RibbonCursorStand(&now);
    TEST_ASSERT(now.last > s.at.last, "a stitch was sewn");
    TEST_ASSERT_EQ(now.sewn_flags & (STITCH_CLEAN | STITCH_DATA_FLUSHED), STITCH_CLEAN | STITCH_DATA_FLUSHED,
                   "and the last says CLEAN, and DATA_FLUSHED with it");
    TEST_ASSERT_EQ(InkedGen(), SettledGen(), "every change settled");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.last, now.last, "on the halt's stitch");
    TEST_ASSERT(said.flags & STITCH_CLEAN, "which the mount reads CLEAN");
out:
    if (shut)
        TagFSStandShutForHalt(false);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// C25, M-6: a refused write is taken again, a new seq, at the next_quire
// ----------------------------------------------------------------------------

/* The volume mounted again, and its ribbon held to a retry: ending on `last`,
 * in the Quire the test found named next, `pieces` the lines for the pieces
 * of a refused attempt, and no lie. */
static bool StandStitchRetrySewn(const StandStitch* s, uint64_t last, uint32_t pieces)
{
    MountBookSaid said;
    RibbonCursor  now;
    if (!StandStitchRemount())
        return false;
    MountBookStandSaid(&said);
    RibbonCursorStand(&now);
    return said.last == last && !said.lied && said.ribbon[RIBBON_CODE_DEAD_BRANCH] == pieces &&
           RibbonQuireSame(&now.quire, &s->at.next);
}

static TestResult TestStandStitchRetryAtom(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    RibbonCursor             now;
    static StandStitchWaiter waiter;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (!s.at.next.blocks)
        return test_steps_aside("the ribbon names no next_quire to go on in");
    memset(&waiter, 0, sizeof(waiter));
    uint64_t vlba = s.at.quire.block * TAGFS_BLOCK_SECTORS + s.at.off;
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, s.base + vlba, s.at.atom, 1);
    SettleWaitLink(&waiter.w, InkedGen(), StandStitchAnswered);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle settles");
    TEST_ASSERT(BlockFailFired(), "though the medium refused its stitch's first atom");
    TEST_ASSERT(waiter.answered && waiter.rc == OK, "and the waiter is answered OK");
    RibbonCursorStand(&now);
    TEST_ASSERT_EQ(now.last, s.at.given + 2u, "by a stitch of a new seq");
    TEST_ASSERT(RibbonQuireSame(&now.sewn.quire, &s.at.next) && now.sewn.off == 0,
                "laid at the start of the next_quire");
    TEST_ASSERT_EQ(now.refused, 0, "and the refusals in a row are none once it settled");
    BlockFailDisarm();
    TEST_ASSERT(StandStitchRetrySewn(&s, now.last, 0), "the mount takes the ribbon on there, and says no lie");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static TestResult TestStandStitchRetryFlush(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor now;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (!s.at.next.blocks)
        return test_steps_aside("the ribbon names no next_quire to go on in");
    BlockFailArm(s.seat, BLOCK_FAIL_FLUSH, 0, 0, 1);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle settles");
    TEST_ASSERT(BlockFailFired(), "though the medium refused the flush after its stitch");
    RibbonCursorStand(&now);
    TEST_ASSERT_EQ(now.last, s.at.given + 2u, "by a stitch of a new seq");
    TEST_ASSERT(RibbonQuireSame(&now.sewn.quire, &s.at.next) && now.sewn.off == 0,
                "laid at the start of the next_quire");
    BlockFailDisarm();
    TEST_ASSERT(StandStitchRetrySewn(&s, now.last, 1),
                "the mount takes the larger seq, says the stitch the flush did not keep is the pieces of a retry, "
                "and says no lie");
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// M-6: no Quire takes the write — the volume may only be read, its waiters
// told; the attempt torn on the medium is the ribbon's torn tail (§3.2, §3.9)
// ----------------------------------------------------------------------------

/* A word of `bytes` bytes of one letter, said to the volume as its `k`-th. */
error_t StandStitchSayLong(uint64_t bytes, char letter, uint64_t k, bool* kept)
{
    char* text = kmalloc(bytes);
    if (!text)
        return ERR_NO_MEMORY;
    memset(text, letter, bytes);
    error_t rc = StandStitchSay(text, bytes, k, kept);
    kfree(text);
    return rc;
}

/* A word whose stitch takes exactly `sectors` sectors — a whole number of
 * atoms — said to the volume as its `k`-th: a stitch's stream fills
 * BOOK_PAYLOAD_BYTES a sector, and its head, its UseContext's tag and length
 * and its Clasp take fewer than STAND_STITCH_SLACK bytes and more than none. */
error_t StandStitchSayFilling(uint64_t sectors, char letter, uint64_t k, bool* kept)
{
    return StandStitchSayLong(sectors * BOOK_PAYLOAD_BYTES - STAND_STITCH_SLACK, letter, k, kept);
}

static TestResult TestStandStitchRefusedEverywhere(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    MountBookSaid            said;
    static StandStitchWaiter waiter;
    bool                     kept  = true;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    uint64_t first = s.at.quire.block * TAGFS_BLOCK_SECTORS + s.at.off;
    uint64_t data  = g_book.frame.data_block * TAGFS_BLOCK_SECTORS;
    uint64_t end   = g_book.data_end * TAGFS_BLOCK_SECTORS;
    BlockFailArmRefusedWrites(s.seat, s.base + first + s.at.atom, end - first - s.at.atom);
    TEST_ASSERT(BlockFailAddBadSectors(s.seat, s.base + data, first - data),
                "every write past the ribbon's next atom, and before it, is refused");
    SettleWaitLink(&waiter.w, InkedGen(), StandStitchAnswered);
    error_t rc = StandStitchSayLong(TALLY_SHEET_PAYLOAD + 16u, 'w', 3, &kept);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_READ_ONLY, "a word two atoms long is refused, once no Quire takes its stitch");
    TEST_ASSERT(!kept, "and the volume does not say it holds it");
    TEST_ASSERT(TagFSReadOnly(), "the volume may only be read from here");
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_TAGFS_READ_ONLY, "and its waiter is told so, not answered OK");
    TEST_ASSERT_EQ(SettleWaitNow(InkedGen() + 1u), ERR_TAGFS_READ_ONLY, "as is every wait after it");
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_EQ(TagFSSettle(), ERR_TAGFS_READ_ONLY, "a cycle asked of it is refused before the Scribe");
    TEST_ASSERT_EQ(TagFSStandSettleForHalt(), ERR_TAGFS_READ_ONLY, "and so are the halt's");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and not one write reaches a medium the kernel said may only be read");
    BlockFailDisarm();
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, as a machine cut off would leave it");
    StandRibbonArrive();
    TagFSStandHoldAgain();
    TEST_ASSERT(tagfs_get_state()->initialized && !TagFSReadOnly(), "mounted again, it takes writes");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.last, s.at.last, "its ribbon ends where it stood: the refused stitch is not sewn on (m-1)");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_RIBBON_TORN], 1, "and its one atom that landed is said, a torn tail");
    TEST_ASSERT(!said.lied, "with no lie said of the medium");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The ribbon runs out with no Tally to take it in, and leaves the Tally's
// ground alone (§3.6, §16.2 п.6)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchRibbonRunsOut(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    TagFSState*              fs      = tagfs_get_state();
    bool                     kept    = false, held = false;
    error_t                  rc      = OK;
    uint64_t                 k       = 10;
    const char*              aside   = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    uint64_t keep = EndpaperKeeps(s.at.quire_blocks);
    TEST_ASSERT(keep >= 2u * s.at.quire_blocks, "the Tally's reserve is two copies of at least one Quire");
    TEST_ASSERT(fs->ledger.free_blocks > keep + s.at.quire_blocks, "the volume has more than that free");
    TEST_ASSERT(BlockMapDerivedStandShrink(keep + s.at.quire_blocks), "its free ground is cut to one Quire past it");
    held = true;
    for (; rc == OK && k < 10u + 3u * s.at.quire_blocks; k++)
        rc = StandStitchSayLong(16u * TALLY_SHEET_PAYLOAD, (char)('a' + k % 26u), k, &kept);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_NO_SPACE, "words are laid down until the ribbon runs out, said ERR_TAGFS_NO_SPACE");
    RibbonCursor now;
    RibbonCursorStand(&now);
    TEST_ASSERT(now.out, "and said aloud");
    TEST_ASSERT(!SettleParked(), "and the cadence is not parked for room nothing on a book asks for");
    uint64_t saying = 0, ticket = 0;
    UseTextDrop(tagfs_recall_use_context(&saying));
    TEST_ASSERT_EQ(UseMemorySay(NULL, saying + 1u, &ticket), USE_SAID_OWED,
                   "a word the volume does not hold is said, as a process's `use` says it");
    SettleWaitLinkCycle(&waiter.w, StandStitchAnswered);
    TEST_ASSERT_OK(TagFSSettleAsCadence(), "the cycle the cadence runs for its waiter lays its stitch without the "
                   "word, in the rest kept");
    TEST_ASSERT(waiter.answered && waiter.rc == OK, "and the waiter is answered");
    TEST_ASSERT(UseMemoryRefused(ticket), "the word is refused: no claim held room for it, and none comes");
    TEST_ASSERT(!now.next.blocks, "the ribbon names no next_quire");
    TEST_ASSERT(fs->ledger.free_blocks >= keep, "and the ground a Tally needs is left free");
    UseText* memory = tagfs_recall_use_context(NULL);
    rc = StandStitchSay(memory ? memory->text : NULL, memory ? memory->bytes : 0, k + 1u, &kept);
    UseTextDrop(memory);
    TEST_ASSERT_OK(rc, "the word the volume holds, said again, is answered: it is what the volume holds");
    TEST_ASSERT(kept && !TagFSReadOnly(), "and the volume holds it, still taking what its rest holds");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    if (held)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The reserve's measure: never less than the Tally a writer lays, and growing
// with what the book holds (§16.2 п.6)
// ----------------------------------------------------------------------------

/* The blocks one copy of the Tally the mount stands on takes, node by node. */
static uint64_t StandStitchTallyBlocks(void)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    uint64_t              blocks  = 0;
    TallyPlace            at;
    for (uint32_t t = 0; t < ARRAY_SIZE(trees); t++)
        for (uint32_t i = 0; TallyStandNodeWhere(trees[t], i, &at); i++)
            blocks += at.ptr.sheets;
    return blocks;
}

static TestResult TestStandStitchTallyCost(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    TagRegistry* reg   = tagfs_get_state()->registry;
    char         text[160];
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    uint64_t laid = StandStitchTallyBlocks();
    uint64_t cost = TallyCostBlocks();
    TestEvidence("blocks of one copy of the Tally the mount stands on:", laid);
    TestEvidence("blocks the reserve measures the next Tally at:", cost);
    TEST_ASSERT(laid > 0 && cost >= laid, "the measure is never less than the Tally the maker laid");
    for (uint32_t i = 0; i < 64u; i++) {
        TagSpelling tag;
        uint32_t    tid = 0;
        uint32_t    n   = (uint32_t)ksnprintf(text, sizeof(text), "stitch:%03u-", i);
        memset(text + n, 'x', sizeof(text) - n - 1u);
        text[sizeof(text) - 1u] = '\0';
        TEST_ASSERT(TagSpell(text, sizeof(text) - 1u, &tag) == OK && TagRegistryIntern(reg, &tag, &tid) == OK,
                    "tags are given ids");
    }
    uint64_t more = TallyCostBlocks();
    TestEvidence("and with 64 tags of 160 bytes more:", more);
    TEST_ASSERT(more > cost, "the measure grows with the tags the book holds");
    TEST_ASSERT(EndpaperKeeps(s.at.quire_blocks) % s.at.quire_blocks == 0, "the reserve is whole Quires");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// MEDIUM_LIED: every stitch after the verdict says it (§16.2 п.10)
// ----------------------------------------------------------------------------

uint32_t StandStitchLayEmpty(const StandStitch* s, uint64_t flags)
{
    uint8_t    body[96];
    uint32_t   starts[2];
    uint64_t   seq = s->at.given + 1u;
    StitchHead h   = { .flags = flags, .prev = s->at.last ? seq - s->at.last : 0, .prev_clasp = s->at.clasp,
                       .tally = seq - s->at.tally, .next_block = s->at.next.block,
                       .next_blocks = s->at.next.blocks };
    if (s->at.came.blocks && s->at.off == 0) {
        h.from_block  = s->at.came.block;
        h.from_blocks = s->at.came.blocks;
    }
    uint32_t at = RibbonRecordPut(s_stitch_stream, RIBBON_STITCH_HEAD, 0, body, StitchHeadBodyPut(body, &h));
    Clasp    c  = { .records = 0, .fresh = 0, .crc = KCrc32c(s_stitch_stream, at) };
    starts[0] = 0;
    starts[1] = at;
    at += RibbonRecordPut(s_stitch_stream + at, RIBBON_CLASP, 0, body, ClaspBodyPut(body, &c));
    uint64_t vlba = s->at.quire.block * TAGFS_BLOCK_SECTORS + s->at.off;
    TallyUnitSeal(s_stitch_sectors, s_stitch_stream, at, starts, 2, BOOK_UNIT_STITCH, seq, vlba,
                  VOLUME_BLOCK_SECTORS, s->uuid);
    return BoardroomWrite(s->seat, s->base + vlba, VOLUME_BLOCK_SECTORS, s_stitch_sectors, NULL) == 0 ? c.crc : 0;
}

static TestResult TestStandStitchMediumLied(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  now;
    MountBookSaid said;
    const char*   aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom != VOLUME_BLOCK_SECTORS)
        return test_steps_aside("the medium's atom is larger than a block: a stitch laid by hand here is one");
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandStitchLayEmpty(&s, STITCH_DATA_FLUSHED | STITCH_MEDIUM_LIED) != 0,
                "a stitch saying the medium lied is laid at the ribbon's end");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on it");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.lied && said.last == s.at.given + 1u, "and says the medium lied");
    TEST_ASSERT(!TagFSIsUnderGlass(), "a medium that lied is written all the same, and said to have lied");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles");
    RibbonCursorStand(&now);
    TEST_ASSERT(now.sewn_flags & STITCH_MEDIUM_LIED, "and its stitch says MEDIUM_LIED in turn");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A medium that leaves and comes back is read again: what no stitch settled is
// lost, and its waiters are told (§16.2 п.28)
// ----------------------------------------------------------------------------

static TestResult TestStandStitchReplugLoses(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    uint64_t                 saying = 0, ticket = 0;
    bool                     away   = false;
    const char*              aside  = StandStitchAside(&s);
    static const char        word[] = "replug,lost";

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    UseText* before = tagfs_recall_use_context(&saying);
    UseText* t      = UseTextMake(word, sizeof(word) - 1u);
    TEST_ASSERT(t != NULL, "a word is made");
    TEST_ASSERT_EQ(UseMemorySay(t, saying + 2000u, &ticket), USE_SAID_OWED, "and said: a cycle owes it the medium");
    UseTextDrop(t);
    SettleWaitLinkCycle(&waiter.w, StandStitchAnswered);
    TagFSStandMediumLeaves();
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(NULL), AWAY_REREAD, "the medium leaves and comes back: read again");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_DEVICE_NOT_READY,
                "the word's waiter is told it is lost, not answered OK");
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, as a book read again is");
    StandRibbonArrive();
    TagFSStandHoldAgain();
    UseText* back = tagfs_recall_use_context(NULL);
    TEST_ASSERT(UseTextSame(back, before), "mounted again, it remembers what its book holds, not the lost word");
    UseTextDrop(back);
out:
    UseTextDrop(before);
    if (away)
        (void)TagFSStandMediumReturns(NULL);
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// BLOCKFAIL: writes refused in two ranges at once, for good (C25, M-6)
// ----------------------------------------------------------------------------

static TestResult TestStandBlockFailRefusedWrites(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    at      = tagfs_get_volume_base() + 64u;
    uint32_t    landed  = 0;
    uint8_t*    was     = g_test_buffer;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(BoardroomRead(seat, at - 4u, 16, was), 0, "sixteen sectors are read, to be written as they are");
    BlockFailArmRefusedWrites(seat, at, 2);
    TEST_ASSERT(BlockFailAddBadSectors(seat, at + 8u, 2), "a second range joins the plan");
    TEST_ASSERT(BoardroomWrite(seat, at + 1u, 1, was + 5u * TAGFS_SECTOR_SIZE, NULL) != 0,
                "a write in the first range is refused");
    TEST_ASSERT(BoardroomWrite(seat, at + 1u, 1, was + 5u * TAGFS_SECTOR_SIZE, NULL) != 0,
                "and again: the plan refuses for good, not once");
    TEST_ASSERT(BoardroomWrite(seat, at + 6u, 4, was + 10u * TAGFS_SECTOR_SIZE, &landed) != 0,
                "a write that runs into the second range is refused");
    TEST_ASSERT_EQ(landed, 2, "from that range's first sector: the two before it land");
    TEST_ASSERT(BoardroomWrite(seat, at - 4u, 6, was, &landed) != 0, "one that runs into the first, refused");
    TEST_ASSERT_EQ(landed, 4, "from its first sector");
    TEST_ASSERT_EQ(BoardroomWrite(seat, at + 4u, 2, was + 8u * TAGFS_SECTOR_SIZE, NULL), 0,
                   "a write between the ranges lands");
    TEST_ASSERT_EQ(BoardroomRead(seat, at, 1, g_test_output), 0, "and a read of a range's sector reads");
    TEST_ASSERT_EQ(BlockFailSeen(), 4, "four writes met the plan");
out:
    BlockFailDisarm();
    return verdict;
}

static const TestCase g_stand_stitch_tests[] = {
    { .name = "stand_stitch_answers_after_flush", .func = TestStandStitchAnswersAfterFlush,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_use_context", .func = TestStandStitchUseContext,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_coined", .func = TestStandStitchCoined,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_halt_clean", .func = TestStandStitchHaltClean,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_atom", .func = TestStandStitchRetryAtom,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_retry_flush", .func = TestStandStitchRetryFlush,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_refused_everywhere", .func = TestStandStitchRefusedEverywhere,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_ribbon_runs_out", .func = TestStandStitchRibbonRunsOut,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_tally_cost", .func = TestStandStitchTallyCost,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_medium_lied", .func = TestStandStitchMediumLied,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_stitch_replug_loses", .func = TestStandStitchReplugLoses,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_blockfail_refused_writes", .func = TestStandBlockFailRefusedWrites,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_stitch_suite = {
    g_stand_stitch_tests, sizeof(g_stand_stitch_tests) / sizeof(g_stand_stitch_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
