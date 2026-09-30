#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "use_context.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The Use Context's chain (step 1.6)
//
// What the volume remembers of its person's Use Context lives in a chain of
// its own the Ledger names (volume_ledger.h, UseChainLink): a context of any
// length is remembered whole and read back off the medium; the chain is ground
// the mount's roll call owns, so a machine that stopped cleanly leaves the
// medium's map and its Ledger agreeing on every block of it; a chain the
// volume stopped remembering goes back only once neither copy of the Ledger
// names it; and a chain that does not read whole rolls the memory back to the
// copy before — saying the medium lied only when a link never landed. What the
// volume remembered before each test is said to it again on every path out,
// and every byte a test changes on the medium is put back.
// ============================================================================

#define STAND_CHAIN_WIDE  65536u   /* bytes of the wide context: 17 links */

/* The copies of the Ledger as the medium holds them, and the newest's index —
 * -1 when neither reads. */
static int StandChainCopies(VolumeLedger led[VOLUME_LEDGER_COPIES])
{
    int newest = -1;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!tagfs_ledger_peek(c, &led[c], NULL)) {
            memset(&led[c], 0, sizeof(led[c]));
            continue;
        }
        if (newest < 0 || led[c].seq > led[newest].seq)
            newest = (int)c;
    }
    return newest;
}

/* A context of exactly `bytes` bytes: tags "use:test:<stem>NNNNNN",
 * comma-joined, the last lengthened with 'z' to end on the byte. */
static UseText* StandChainText(const char* stem, size_t bytes)
{
    UseText* t   = UseTextRoom(bytes);
    size_t   pos = 0;
    char     tag[48];
    if (!t)
        return NULL;
    for (uint32_t i = 0; ; i++) {
        int n = ksnprintf(tag, sizeof(tag), "%suse:test:%s%06u", pos ? "," : "", stem, i);
        if (pos + (size_t)n + 1u > bytes)
            break;
        memcpy(t->text + pos, tag, (size_t)n);
        pos += (size_t)n;
    }
    memset(t->text + pos, 'z', bytes - pos);
    return t;
}

/* The chain the newest copy names, as the medium holds it. */
static bool StandChainNewest(UseChain* out)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    UseChainRef  ref;
    int          newest = StandChainCopies(led);
    memset(out, 0, sizeof(*out));
    if (newest < 0 || !UseChainRefOf(&led[newest], &ref) || ref.first == VOLUME_LEDGER_NO_BLOCK)
        return false;
    return UseChainRead(&ref, NULL, out) == USE_CHAIN_WHOLE;
}

/* Whether any copy on the medium names the chain whose first link is `first`. */
static bool StandChainNamedOnMedium(uint32_t first)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    UseChainRef  ref;
    (void)StandChainCopies(led);
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        if (led[c].magic[0] && UseChainRefOf(&led[c], &ref) && ref.first == first)
            return true;
    return false;
}

/* How many blocks of `chain` the map in memory calls taken. */
static uint64_t StandChainTaken(const UseChain* chain)
{
    uint64_t taken = 0;
    for (uint32_t r = 0; r < chain->run_count; r++)
        for (uint32_t k = 0; k < chain->runs[r].block_count; k++)
            taken += BlockMapIsTaken(chain->runs[r].start_block + k);
    return taken;
}

static uint64_t StandChainBlocks(const UseChain* chain)
{
    uint64_t blocks = 0;
    for (uint32_t r = 0; r < chain->run_count; r++)
        blocks += chain->runs[r].block_count;
    return blocks;
}

/* Whether the medium's own map calls `block` taken: read off the medium. */
static bool StandChainTakenOnMedium(uint32_t block, bool* taken)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    vlba = (uint64_t)fs->layout.block_bitmap_block * TAGFS_BLOCK_SECTORS +
                       block / BLOCK_MAP_WINDOW_BLOCKS;
    uint32_t    bit  = block % BLOCK_MAP_WINDOW_BLOCKS;
    if (tagfs_volume_read(vlba, 1, g_test_output) != 0)
        return false;
    *taken = (g_test_output[bit / 8u] >> (bit % 8u)) & 1u;
    return true;
}

/* How many blocks of `chain` the medium's map calls taken; false when the map
 * would not read. */
static bool StandChainTakenOnMediumCount(const UseChain* chain, uint64_t* taken)
{
    *taken = 0;
    for (uint32_t r = 0; r < chain->run_count; r++)
        for (uint32_t k = 0; k < chain->runs[r].block_count; k++) {
            bool on = false;
            if (!StandChainTakenOnMedium(chain->runs[r].start_block + k, &on))
                return false;
            *taken += on;
        }
    return true;
}

static uint64_t StandFallowBlocks(void)
{
    TagFSLock* lock = &tagfs_get_state()->lock;
    TagFSLockTake(lock);
    uint64_t resting = FallowBlocks();
    TagFSLockDrop(lock);
    return resting;
}

/* What the volume held before the test, said to it again. */
static TestResult StandChainPutBack(TestResult verdict, UseText* before, uint64_t saying)
{
    bool    kept = false;
    error_t rc   = tagfs_remember_use_context(before, saying, &kept);
    bool    had  = before != NULL;
    UseTextDrop(before);
    if (rc != OK || (had && !kept))
        return TestFailedAt(__FILE__, __LINE__, "the earlier context is put back and "
                            "remembered again (error, remembered)", rc, kept);
    return verdict;
}

// ----------------------------------------------------------------------------
// 64 KiB, whole, off the medium
// ----------------------------------------------------------------------------

/* A context of 64 KiB is remembered, and read back off the medium it is the
 * same text, in ceil(65536 / 4052) = 17 links the Ledger names — none of it in
 * the Ledger's own sector. */
static TestResult TestStandUseChainWide(void)
{
    TestResult   verdict = TEST_PASS;
    uint64_t     saying  = 0;
    UseText*     before  = tagfs_recall_use_context(&saying);
    UseText*     wide    = StandChainText("wide", STAND_CHAIN_WIDE);
    UseText*     medium  = NULL;
    UseChain     chain;
    bool         changed = false, kept = false;
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    int          newest;

    memset(&chain, 0, sizeof(chain));
    TEST_ASSERT(wide != NULL, "memory for a context of 64 KiB");
    changed = true;
    TEST_ASSERT_OK(tagfs_remember_use_context(wide, saying, &kept), "the volume takes it");
    TEST_ASSERT(kept, "and remembers it");

    newest = StandChainCopies(led);
    TEST_ASSERT(newest >= 0, "a copy of the Ledger reads");
    TEST_ASSERT(tagfs_ledger_peek((uint32_t)newest, &led[newest], &medium),
                "the newest copy reads again");
    TEST_ASSERT(medium != NULL, "the chain it names reads back whole");
    TEST_ASSERT_EQ(medium->bytes, STAND_CHAIN_WIDE, "every byte of it");
    TEST_ASSERT(memcmp(medium->text, wide->text, STAND_CHAIN_WIDE) == 0, "as it was said");
    TEST_ASSERT_EQ(led[newest].use_chain_bytes, STAND_CHAIN_WIDE, "the Ledger names its length");
    TEST_ASSERT_EQ(led[newest].use_context_bytes, 0, "and keeps no text in its sector");

    TEST_ASSERT(StandChainNewest(&chain), "the chain is followed on the medium");
    TestEvidence("links of a 65536-byte Use Context on the medium", StandChainBlocks(&chain));
    TEST_ASSERT_EQ(StandChainBlocks(&chain), 17, "in 17 links");
    TEST_ASSERT_EQ(StandChainTaken(&chain), 17, "every one of them taken on the map");
out:
    UseChainForget(&chain);
    UseTextDrop(medium);
    UseTextDrop(wide);
    if (changed)
        verdict = StandChainPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

// ----------------------------------------------------------------------------
// The roll call owns it: no orphan, no missing block
// ----------------------------------------------------------------------------

/*
 * `use` twice, then the cycles a stopping machine runs (TagFSSettleForHalt's
 * rule): the medium's map calls every block of the chain the Ledger names
 * taken, and every block of the chain it replaced free — so the next mount's
 * census finds neither an orphan nor a missing block of the Use Context
 * ("Bitmap mismatch"). Before the chain was the roll call's own, every mount
 * named a live chain's blocks orphans.
 */
static TestResult TestStandUseChainNoOrphan(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   saying  = 0;
    UseText*   before  = tagfs_recall_use_context(&saying);
    UseText*   first   = StandChainText("roll", 5000);
    UseText*   second  = StandChainText("call", 300);
    UseChain   x, y;
    bool       changed = false, kept = false;
    uint64_t   taken   = 0;
    uint32_t   rounds  = 0;

    memset(&x, 0, sizeof(x));
    memset(&y, 0, sizeof(y));
    TEST_ASSERT(first && second, "memory for two contexts");
    changed = true;
    TEST_ASSERT_OK(tagfs_remember_use_context(first, saying, &kept), "the first context is said");
    TEST_ASSERT(kept && StandChainNewest(&x), "and remembered in a chain");
    TEST_ASSERT_OK(tagfs_remember_use_context(second, saying, &kept), "the second is said");
    TEST_ASSERT(kept && StandChainNewest(&y), "and remembered in another");
    TEST_ASSERT(x.ref.first != y.ref.first, "the two chains are two");

    while (StandFallowBlocks() > 0 && rounds++ <= FallowLag() + 1u)
        TEST_ASSERT_OK(TagFSSettle(), "a cycle settles while ground rests");
    TEST_ASSERT_OK(TagFSSettle(), "and one more writes the map of what came back");

    TEST_ASSERT(StandChainNamedOnMedium(y.ref.first), "the medium's Ledger names the second chain");
    TEST_ASSERT(!StandChainNamedOnMedium(x.ref.first), "and no copy names the first any more");
    TEST_ASSERT(StandChainTakenOnMediumCount(&y, &taken), "the medium's map reads");
    TestEvidence("blocks of the named chain the medium's map calls free", StandChainBlocks(&y) - taken);
    TEST_ASSERT_EQ(taken, StandChainBlocks(&y), "every block of the named chain is taken on the medium");
    TEST_ASSERT(StandChainTakenOnMediumCount(&x, &taken), "the medium's map reads");
    TEST_ASSERT_EQ(taken, 0, "and every block of the replaced one is free there");
out:
    UseChainForget(&x);
    UseChainForget(&y);
    UseTextDrop(first);
    UseTextDrop(second);
    if (changed)
        verdict = StandChainPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

// ----------------------------------------------------------------------------
// Two changes in a row
// ----------------------------------------------------------------------------

/* The chain's blocks stay taken while any copy on the medium names it. */
static bool StandChainHeldWhileNamed(const UseChain* chain)
{
    return !StandChainNamedOnMedium(chain->ref.first) ||
           StandChainTaken(chain) == StandChainBlocks(chain);
}

/*
 * The context changes twice in a row, X to Y to Z: after every cycle, a chain
 * any copy of the Ledger on the medium names keeps every block taken — the
 * first chain goes back only once both copies stopped naming it, and the
 * second, named by one copy while the other names the third, stays.
 */
static TestResult TestStandUseChainNamedByBoth(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   saying  = 0;
    UseText*   before  = tagfs_recall_use_context(&saying);
    UseText*   tx      = StandChainText("both", 6000);
    UseText*   ty      = StandChainText("copy", 6000);
    UseText*   tz      = StandChainText("name", 100);
    UseChain   x, y;
    bool       changed = false, kept = false;
    uint32_t   rounds  = 0;

    memset(&x, 0, sizeof(x));
    memset(&y, 0, sizeof(y));
    TEST_ASSERT(tx && ty && tz, "memory for three contexts");
    changed = true;
    TEST_ASSERT_OK(tagfs_remember_use_context(tx, saying, &kept), "X is said");
    TEST_ASSERT(kept && StandChainNewest(&x), "and remembered");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle more: both copies name X");

    TEST_ASSERT_OK(tagfs_remember_use_context(ty, saying, &kept), "Y is said");
    TEST_ASSERT(kept && StandChainNewest(&y), "and remembered");
    TEST_ASSERT(StandChainNamedOnMedium(x.ref.first), "the other copy still names X");
    TEST_ASSERT(StandChainHeldWhileNamed(&x), "and X's blocks are all taken");

    TEST_ASSERT_OK(tagfs_remember_use_context(tz, saying, &kept), "Z is said");
    TEST_ASSERT(kept, "and remembered");
    TEST_ASSERT(StandChainNamedOnMedium(y.ref.first), "one copy names Y, the other Z");
    TEST_ASSERT(StandChainHeldWhileNamed(&y), "and Y's blocks are all taken");

    while ((StandChainTaken(&x) || StandChainTaken(&y)) && rounds++ <= FallowLag() + 3u) {
        TEST_ASSERT_OK(TagFSSettle(), "a cycle settles");
        TEST_ASSERT(StandChainHeldWhileNamed(&x), "X keeps its blocks while a copy names it");
        TEST_ASSERT(StandChainHeldWhileNamed(&y), "Y keeps its blocks while a copy names it");
    }
    TestEvidence("cycles until neither replaced chain holds a block", rounds);
    TEST_ASSERT_EQ(StandChainTaken(&x), 0, "X goes back once no copy names it");
    TEST_ASSERT_EQ(StandChainTaken(&y), 0, "and Y too");
out:
    UseChainForget(&x);
    UseChainForget(&y);
    UseTextDrop(tx);
    UseTextDrop(ty);
    UseTextDrop(tz);
    if (changed)
        verdict = StandChainPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

// ----------------------------------------------------------------------------
// A link that does not read whole
// ----------------------------------------------------------------------------

/* What a mount of the medium as it is now would remember. */
static void StandChainJudge(UseMemoryVerdict* v)
{
    static VolumeLedger led[VOLUME_LEDGER_COPIES];
    const VolumeLedger* rec[VOLUME_LEDGER_COPIES];
    UseText* const      none[VOLUME_LEDGER_COPIES] = { NULL, NULL };
    int                 newest = StandChainCopies(led);
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        rec[c] = led[c].magic[0] ? &led[c] : NULL;
    UseMemoryJudge(rec, newest < 0 ? 0u : (uint32_t)newest, none, v);
}

static int StandChainBlockIo(uint32_t block, void* buf, bool write)
{
    uint64_t vlba = ((uint64_t)tagfs_get_state()->layout.data_block + block) * TAGFS_BLOCK_SECTORS;
    return write ? tagfs_volume_write(vlba, TAGFS_BLOCK_SECTORS, buf)
                 : tagfs_volume_read(vlba, TAGFS_BLOCK_SECTORS, buf);
}

/*
 * The newest copy names a chain whose second link lost a byte of its text:
 * a mount rolls the memory back to what the copy before it remembers, and
 * does not say the medium lied — the link is its own chain's, rot is not a
 * broken FLUSH. The same link's block holding another magic — the link never
 * landed — is the lie, and a mount says so. Put back, the chain reads whole.
 */
static TestResult TestStandUseChainRotRollsBack(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         saying  = 0;
    UseText*         before  = tagfs_recall_use_context(&saying);
    UseText*         older   = StandChainText("aged", 6000);
    UseText*         newer   = StandChainText("rot", 6000);
    UseChain         chain;
    UseMemoryVerdict v;
    bool             changed = false, kept = false, broken = false;
    uint32_t         link0 = 0, link1 = 0;
    uint8_t*         saved = g_test_buffer;
    uint8_t*         work  = g_test_buffer + TAGFS_BLOCK_SIZE;
    uint8_t*         saved0 = g_test_buffer + 2u * TAGFS_BLOCK_SIZE;

    memset(&chain, 0, sizeof(chain));
    memset(&v, 0, sizeof(v));
    TEST_ASSERT(older && newer, "memory for two contexts");
    changed = true;
    TEST_ASSERT_OK(tagfs_remember_use_context(older, saying, &kept), "the older context is said");
    TEST_ASSERT(kept, "and remembered");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle more: both copies name it");
    TEST_ASSERT_OK(tagfs_remember_use_context(newer, saying, &kept), "the newer is said");
    TEST_ASSERT(kept && StandChainNewest(&chain), "and remembered in a chain of its own");
    TEST_ASSERT(StandChainBlocks(&chain) == 2, "two links");

    link0 = chain.runs[0].start_block;
    link1 = chain.runs[0].block_count > 1 ? link0 + 1u : chain.runs[1].start_block;
    TEST_ASSERT(StandChainBlockIo(link1, saved, false) == 0, "the second link reads");
    TEST_ASSERT(StandChainBlockIo(link0, saved0, false) == 0, "the first link reads");

    memcpy(work, saved, TAGFS_BLOCK_SIZE);
    work[__builtin_offsetof(UseChainLink, text) + 7u] ^= 0x5Au;
    broken = true;
    TEST_ASSERT(StandChainBlockIo(link1, work, true) == 0, "a byte of its text is changed");
    StandChainJudge(&v);
    TestEvidence("a rotten link: the mount's memory came from (1 newest, 2 older)", v.from);
    TEST_ASSERT(v.rolled && v.newest_state == USE_CHAIN_ROTTEN, "the newest chain is rotten");
    TEST_ASSERT_EQ(v.from, USE_FROM_OLDER, "the memory rolls back to the copy before");
    TEST_ASSERT(v.text && UseTextSame(v.text, older), "to the older context, whole");
    TEST_ASSERT(!v.lied, "and the medium is not called a liar");
    UseMemoryVerdictDrop(&v);

    TEST_ASSERT(StandChainBlockIo(link1, saved, true) == 0, "the second link is put back");
    memcpy(work, saved0, TAGFS_BLOCK_SIZE);
    memset(work, 0, sizeof(uint32_t));
    TEST_ASSERT(StandChainBlockIo(link0, work, true) == 0, "the first link's magic is not its own");
    StandChainJudge(&v);
    TEST_ASSERT(v.rolled && v.newest_state == USE_CHAIN_FOREIGN, "a link that never landed");
    TEST_ASSERT_EQ(v.from, USE_FROM_OLDER, "rolls back the same way");
    TEST_ASSERT(v.lied, "and is the medium's lie about its FLUSH");
    UseMemoryVerdictDrop(&v);

    TEST_ASSERT(StandChainBlockIo(link0, saved0, true) == 0, "the first link is put back");
    broken = false;
    StandChainJudge(&v);
    TEST_ASSERT(!v.rolled && v.from == USE_FROM_NEWEST && v.text && UseTextSame(v.text, newer),
                "put back, the newest chain reads whole");
out:
    if (broken) {
        (void)StandChainBlockIo(link1, saved, true);
        (void)StandChainBlockIo(link0, saved0, true);
    }
    UseMemoryVerdictDrop(&v);
    UseChainForget(&chain);
    UseTextDrop(older);
    UseTextDrop(newer);
    if (changed)
        verdict = StandChainPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_use_chain_tests[] = {
    { .name  = "stand_use_chain_wide", .func = TestStandUseChainWide,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_use_chain_no_orphan", .func = TestStandUseChainNoOrphan,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_use_chain_named_by_both", .func = TestStandUseChainNamedByBoth,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_use_chain_rot_rolls_back", .func = TestStandUseChainRotRollsBack,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_use_chain_suite = {
    .tests = g_stand_use_chain_tests, .count = ARRAY_SIZE(g_stand_use_chain_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
