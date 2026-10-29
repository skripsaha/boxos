#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../metadata_pool/meta_pool.h"
#include "../mount/mount.h"
#include "../mount/away.h"
#include "../ink/fallow.h"
#include "../ledger/ledger.h"
#include "../settle/settle.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "boardroom.h"
#include "cpu_calibrate.h"
#include "block_fail.h"
#include "../../../lib/kernel/crypto.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The seal a Settle cycle leaves, and the mount that stands on it (step 1.6)
//
// Every cycle's S4 seals the Ledger copy it writes with what the cycle left on
// the medium (volume_ledger.h, SettleMark), and a mount stands on the newest
// seal the medium bears out (metadata_pool/meta_pool.h). A mount cannot be
// made from inside a mounted volume, so these ask the mount's own judge — the
// walk and the rules PoolMount uses, reading only — of the medium as it is,
// of the medium after a cycle cut at its pool and at its Ledger, and of seals
// the medium does not bear out; and they ask the file table's walk of a chain
// torn under a seal, and the rule that derives the next file id.
// ============================================================================

/* The copies of the Ledger as the medium holds them: the newest intact one,
 * the other when it is intact too. False when neither reads. */
static bool StandSealCopies(VolumeLedger* newest, VolumeLedger* other, bool* has_other)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    bool         ok[VOLUME_LEDGER_COPIES];
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        ok[c] = tagfs_ledger_peek(c, &led[c], NULL);

    uint32_t n = ok[1] && (!ok[0] || led[1].seq > led[0].seq) ? 1u : 0u;
    if (!ok[n])
        return false;
    *newest    = led[n];
    *has_other = ok[1u - n];
    if (*has_other)
        *other = led[1u - n];
    return true;
}

/* A copy's own seal (the mount's rule, ledger.c LedgerCopySeal). */
static bool StandSealOf(const VolumeLedger* led)
{
    return LedgerHas(led, VOLUME_LEDGER_SEAL_END) && led->mark.settle != 0 &&
           led->mark.settle == led->seq;
}

static uint32_t StandPoolHead(void)
{
    TagFSState* fs = tagfs_get_state();
    return fs->layout.metadata_pool_block - fs->layout.data_block;
}

/* The medium's pool against the newest seal, and the other copy's: what a
 * mount would stand on, and whether it would mark the medium
 * SETTLE_MEDIUM_LIED (NULL: not asked). */
static PoolStood StandSealJudge(const VolumeLedger* newest, const VolumeLedger* other,
                                bool has_other, int* seal, bool* fell_back, bool* lied)
{
    bool         said = false;
    PoolMountAsk ask = {
        .seal = { &newest->mark, has_other && StandSealOf(other) ? &other->mark : NULL },
        .head = StandPoolHead(),
    };
    PoolStood stood = PoolSealsJudge(&ask, seal, fell_back, &said);
    if (lied)
        *lied = said;
    return stood;
}

/* A settled cycle's seal names the pool and the registry as the medium holds
 * them: the mount stands on it, and falls back to nothing. */
static TestResult TestStandSealBorne(void)
{
    TestResult   verdict = TEST_PASS;
    VolumeLedger newest, other;
    bool         has_other = false, fell_back = true;
    int          seal = -1;
    uint8_t*     block = NULL;

    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle settles");
    TEST_ASSERT(StandSealCopies(&newest, &other, &has_other), "a copy of the Ledger reads");
    TEST_ASSERT(StandSealOf(&newest), "the newest copy carries a seal of its own");
    TEST_ASSERT(newest.use_context_bytes == 0 && newest.use_context_offset == 0,
                "and no text of a Use Context is laid in the Ledger's sector");

    PoolStood stood = StandSealJudge(&newest, &other, has_other, &seal, &fell_back, NULL);
    TEST_ASSERT_EQ(stood, POOL_STOOD_SEAL, "the mount would stand on a seal");
    TEST_ASSERT_EQ(seal, 0, "the newest one");
    TEST_ASSERT(!fell_back, "without falling back");

    block = kmalloc(TAGFS_BLOCK_SIZE);
    TEST_ASSERT(block, "memory for the registry's block");
    TEST_ASSERT_OK(TagFSMetaBlockRead(newest.mark.registry_block, block),
                   "the registry block the seal names reads");
    const TagRegistryBlock* reg = (const TagRegistryBlock*)block;
    TEST_ASSERT(newest.mark.registry_bytes <= reg->used_bytes, "and holds at least what it says");
    TEST_ASSERT_EQ(KCrc32(reg->data, newest.mark.registry_bytes), newest.mark.registry_crc32,
                   "and its records sum to the seal's CRC32");
out:
    kfree(block);
    return verdict;
}

/* Arm a refusal of the first write into `blocks` volume blocks from `vblock`. */
static void StandSealRefuseWrite(uint64_t vblock, uint64_t blocks)
{
    uint64_t sector = tagfs_get_volume_base() + vblock * TAGFS_BLOCK_SECTORS;
    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, sector, blocks * TAGFS_BLOCK_SECTORS, 1);
}

/*
 * A cycle cut at its pool (S3) and at its Ledger (S4) — the medium refusing
 * the write, as a cut leaves it — declares nothing, and the seal the medium
 * holds is still the last settled one, borne out: the cut cycle's records lie
 * past its `to`, or in its unwritten pool. Then the cycle settles, and the new
 * seal is borne out in its turn.
 */
static TestResult TestStandSealCutKeepsTheSeal(void)
{
    TestResult   verdict = TEST_PASS;
    VolumeLedger before, after, other;
    bool         has_other = false, fell_back = true;
    int          seal = -1;
    uint64_t     file_id = 0;
    TagFSState*  fs = tagfs_get_state();
    const char*  aside = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(TagFSSettle(), "the volume settles first");
    TEST_ASSERT(StandSealCopies(&before, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(StandSealOf(&before), "and is sealed");

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5E, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made, and no cycle has settled it");

    StandSealRefuseWrite(fs->layout.data_block + before.mark.pool_to_block, 1);
    error_t cut = TagFSSettle();
    BlockFailDisarm();
    TEST_ASSERT(BlockFailFired(), "the cycle's write of the seal's open block was refused");
    TEST_ASSERT(cut != OK, "and the cycle cut at its pool is refused");
    TEST_ASSERT(StandSealCopies(&after, &other, &has_other), "the Ledger reads");
    TEST_ASSERT_EQ(after.seq, before.seq, "no copy moved past the cut");
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back, NULL),
                   POOL_STOOD_SEAL,
                   "the mount would stand on the seal the cut left");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest, borne out");

    StandSealRefuseWrite(fs->layout.state_block, VOLUME_LEDGER_COPIES);
    cut = TagFSSettle();
    BlockFailDisarm();
    TEST_ASSERT(BlockFailFired(), "the cycle's Ledger write was refused");
    TEST_ASSERT(cut != OK, "and the cycle cut at its Ledger is refused");
    TEST_ASSERT(StandSealCopies(&after, &other, &has_other), "the Ledger reads");
    TEST_ASSERT_EQ(after.seq, before.seq, "no copy moved past the cut");
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back, NULL),
                   POOL_STOOD_SEAL,
                   "the pool the cut cycle wrote past the seal still bears it out");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest, borne out");

    TEST_ASSERT_OK(TagFSSettle(), "with the medium answering, the cycle settles");
    TEST_ASSERT(StandSealCopies(&after, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(after.seq > before.seq, "a newer copy landed");
    TEST_ASSERT(StandSealOf(&after), "sealed");
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back, NULL),
                   POOL_STOOD_SEAL,
                   "and its seal is borne out");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest");
out:
    BlockFailDisarm();
    if (file_id) {
        tagfs_delete_file(file_id, INK_UNWARRANTED);
        (void)TagFSSettle();
    }
    return verdict;
}

/*
 * The medium said a flush was kept and was not: the newest seal names pool
 * records the medium does not hold. The mount falls back to the copy before
 * it (and marks the medium SETTLE_MEDIUM_LIED); a seal whose closed part is
 * not borne out falls back the same way; an open block neither seal bears out
 * rolls back to the last closed block (decision 5), marked the same; and no
 * closed part borne out is the road of a kernel before the seal, or rot, which
 * marks nothing.
 */
static TestResult TestStandSealLieFallsBack(void)
{
    TestResult   verdict = TEST_PASS;
    VolumeLedger real, other, lied, torn;
    bool         has_other = false, fell_back = false, marked = false;
    int          seal = -1;

    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle settles");
    TEST_ASSERT(StandSealCopies(&real, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(StandSealOf(&real), "and is sealed");

    /* The newest cycle's open block as the lie leaves it: short of the seal. */
    lied = real;
    lied.seq++;
    lied.mark.settle        = lied.seq;
    lied.mark.pool_to_bytes = real.mark.pool_to_bytes + 42u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &real, true, &seal, &fell_back, &marked), POOL_STOOD_SEAL,
                   "an open block short of the newest seal: the mount stands on a seal");
    TEST_ASSERT_EQ(seal, 1, "the one before it");
    TEST_ASSERT(fell_back, "and says it fell back");
    TEST_ASSERT(marked, "marking the medium SETTLE_MEDIUM_LIED");

    /* Its closed blocks as the lie leaves them. */
    lied = real;
    lied.seq++;
    lied.mark.settle        = lied.seq;
    lied.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &real, true, &seal, &fell_back, &marked), POOL_STOOD_SEAL,
                   "closed blocks the newest seal does not find: the mount stands on a seal");
    TEST_ASSERT(seal == 1 && fell_back && marked, "the one before it, falling back, marked");

    /* Neither seal's open block borne out, their closed blocks both: torn. */
    lied = real;
    lied.seq++;
    lied.mark.settle      = lied.seq;
    lied.mark.tail_crc32 ^= 1u;
    torn = real;
    torn.mark.tail_crc32 ^= 2u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &torn, true, &seal, &fell_back, &marked),
                   POOL_STOOD_TORN_TAIL, "an open block neither seal finds rolls back");
    TEST_ASSERT(seal == 0 && !fell_back, "under the newest seal, without falling back");
    TEST_ASSERT(marked, "and the roll back marks the medium as any other");

    /* No seal's closed part borne out: the road of a kernel before the seal. */
    lied.mark.closed_crc32 ^= 1u;
    torn.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &torn, true, &seal, &fell_back, &marked),
                   POOL_STOOD_NO_SEAL, "closed blocks no seal finds: the whole chain, unsealed");
    TEST_ASSERT_EQ(seal, -1, "on no seal");
    TEST_ASSERT(!marked, "and rot, or a kernel before the seal, marks nothing");
out:
    return verdict;
}

/*
 * R11 (Z11-3, the owner's B3). The newest cycle closed the other seal's open
 * block and settled; then its own open block was torn. The mount stands on the
 * newest seal and rolls back that open block alone: falling back to the other
 * seal would lose every record the newest cycle laid in the block it closed,
 * all of them behind its flush. Red before: the other seal, borne out, was
 * fallen back to. And R11 (Z09-6): a newest seal alone — the other copy
 * written by a kernel before the seal — whose open block is not borne out
 * rolls nothing back, since that block may hold the records such a kernel kept
 * there; the whole chain is read, the medium marked. Red before: the open
 * block was rolled back on the one seal.
 */
static TestResult TestStandSealRollbackStandsOnTheNewest(void)
{
    TestResult     verdict = TEST_PASS;
    VolumeLedger   real, other, torn, older;
    bool           has_other = false, fell_back = true, marked = false;
    int            seal = -1;
    MetaPoolBlock* first = kmalloc(sizeof(MetaPoolBlock));

    TEST_ASSERT(first, "memory for the pool's first block");
    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle settles");
    TEST_ASSERT(StandSealCopies(&real, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(StandSealOf(&real), "and is sealed");
    TEST_ASSERT(real.mark.chain_first != real.mark.pool_to_block,
                "the sealed pool has a closed block before its open one");
    TEST_ASSERT_OK(TagFSMetaBlockRead(real.mark.chain_first, first), "its first block reads");
    TEST_ASSERT(first->magic == TAGFS_MPOOL_MAGIC && first->used_bytes <= TAGFS_MPOOL_DATA_SIZE,
                "as a block of the pool");

    /* The cycle before, whose open block was that first block as it is now. */
    older = real;
    older.seq--;
    older.mark.settle        = older.seq;
    older.mark.pool_to_block = real.mark.chain_first;
    older.mark.pool_to_bytes = first->used_bytes;
    older.mark.closed_crc32  = 0;
    older.mark.tail_crc32    = KCrc32(first->payload, first->used_bytes);
    torn = real;
    torn.mark.tail_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&older, &other, false, &seal, &fell_back, &marked),
                   POOL_STOOD_SEAL, "that older seal is borne out on its own");
    TEST_ASSERT_EQ(StandSealJudge(&torn, &older, true, &seal, &fell_back, &marked),
                   POOL_STOOD_TORN_TAIL,
                   "the newest torn beside it: its open block rolls back");
    TEST_ASSERT(seal == 0 && !fell_back, "under the newest seal, without falling back");
    TEST_ASSERT(marked, "marking the medium SETTLE_MEDIUM_LIED");

    /* The newest seal alone. */
    TEST_ASSERT_EQ(StandSealJudge(&torn, &older, false, &seal, &fell_back, &marked),
                   POOL_STOOD_NO_SEAL,
                   "the newest alone, its open block torn: the whole chain, nothing rolled back");
    TEST_ASSERT(seal == -1 && !fell_back, "on no seal");
    TEST_ASSERT(marked, "marking the medium SETTLE_MEDIUM_LIED");
    torn = real;
    torn.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&torn, &older, false, &seal, &fell_back, &marked),
                   POOL_STOOD_NO_SEAL, "the newest alone, its closed blocks not borne out");
    TEST_ASSERT(!marked, "is rot or a kernel before the seal, and marks nothing");
out:
    kfree(first);
    return verdict;
}

/*
 * A table block torn under the seal still mounts: the table is derived from
 * the pool, so its walk ends at the torn block and the volume stands — while
 * a volume with no seal, whose table says which files there are, does not.
 * The chain is a scratch one of two blocks, the second one garbage.
 */
static TestResult TestStandSealTornTableStands(void)
{
    TestResult      verdict = TEST_PASS;
    uint64_t        head = 0, torn = 0;
    FileTableBlock* image = kmalloc(sizeof(FileTableBlock));
    const char*     aside = StandStepsAsideFor();

    if (aside) {
        kfree(image);
        return test_steps_aside(aside);
    }
    TEST_ASSERT(image, "memory for a table block");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(1, &head), 0, "a block for the scratch chain's head");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(1, &torn), 0, "and one for its torn second block");

    memset(image, 0xA5, sizeof(*image));
    TEST_ASSERT_OK(TagFSMetaBlockWrite(torn, image), "the second block is garbage");
    memset(image, 0, sizeof(*image));
    image->magic      = TAGFS_FILETBL_MAGIC;
    image->next_block = (uint32_t)torn;    /* the table's chain is format v1's */
    TEST_ASSERT_OK(TagFSMetaBlockWrite(head, image), "the head links it");

    TEST_ASSERT(FileTableWouldStand((uint32_t)head, true), "under a seal the torn table still mounts");
    TEST_ASSERT(!FileTableWouldStand((uint32_t)head, false), "with no seal it does not");

    /* A read of the second block the medium refuses says nothing of what it
     * holds, under a seal too; a sector of it the medium cannot read is what
     * it holds. */
    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_READ, tagfs_block_to_sector(torn),
                 TAGFS_BLOCK_SECTORS, 1);
    TEST_ASSERT(!FileTableWouldStand((uint32_t)head, true),
                "a read of it the medium refuses is no verdict: under a seal the walk is refused");
    TEST_ASSERT(BlockFailFired(), "the read was refused");
    BlockFailArmBadSectors(tagfs_get_seat(), tagfs_block_to_sector(torn), TAGFS_BLOCK_SECTORS);
    TEST_ASSERT(FileTableWouldStand((uint32_t)head, true),
                "a sector of it the medium cannot read is what it holds: under a seal it mounts");
    TEST_ASSERT(BlockFailFired(), "the read met that sector");
out:
    BlockFailDisarm();
    if (torn)
        tagfs_free_blocks(torn, 1);
    if (head)
        tagfs_free_blocks(head, 1);
    kfree(image);
    return verdict;
}

/*
 * The next file id is derived: past every id a row or a record of the medium
 * names, never short of the Ledger's count, a wrapped count left for the issue
 * to refuse — and on the volume up now, every live file is below it.
 */
static TestResult TestStandSealFidBoundaryDerived(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   batch[64];
    uint64_t   from = 1, ceiling;
    uint32_t   n;

    TEST_ASSERT_EQ(MountNextFileId(10, 15, 3), 16, "a row past the Ledger's count moves it");
    TEST_ASSERT_EQ(MountNextFileId(10, 5, 20), 21, "a record past it does too");
    TEST_ASSERT_EQ(MountNextFileId(30, 5, 20), 30, "a count past every id stands");
    TEST_ASSERT_EQ(MountNextFileId(0, 5, 5), 0, "a wrapped count is left for the issue to refuse");
    TEST_ASSERT_EQ(MountNextFileId(7, 0xFFFFFFFEu, 0), 0xFFFFFFFFu, "the last id seen: every id issued");
    TEST_ASSERT_EQ(MountNextFileId(7, 0xFFFFFFFFu, 0), 0xFFFFFFFFu, "and no further");

    ceiling = tagfs_file_ceiling();
    TEST_ASSERT(ceiling != 0, "the volume is up");
    while ((n = FileMapRows(tagfs_get_state()->files, from, batch, ARRAY_SIZE(batch))) > 0) {
        for (uint32_t i = 0; i < n; i++)
            TEST_ASSERT(batch[i] < ceiling, "every live file's id is below the next one");
        if (batch[n - 1] == UINT64_MAX)
            break;
        from = batch[n - 1] + 1;
    }
out:
    return verdict;
}

// ============================================================================
// A sector the medium cannot read, judged by where it lies (the owner,
// 2026-10-07): the volume let go of, a BLOCKFAIL plan answering every read of
// one block of its pool as a sector the medium cannot read, and its next
// arrival attended — the mount itself, start to end.
// ============================================================================

#define STAND_SEAL_BARE_CYCLES  8u
#define STAND_SEAL_MOUNT_MS     10000u

static uint64_t StandSealResting(void)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint64_t resting = FallowBlocks();
    TagFSLockDrop(&fs->lock);
    return resting;
}

/*
 * Cycles until nothing rests in Fallow, and one more, whose map says on the
 * medium that the ground came back: a take-down drops what rests without a
 * word, and the next mount would read it as ground nobody owns. A cycle that
 * leaves nothing resting took nothing off the chain's front, and the one after
 * it neither — nothing is inked between — so both copies' seals begin the
 * chain at one block.
 */
static bool StandSealBare(void)
{
    for (uint32_t cycle = 0; cycle < STAND_SEAL_BARE_CYCLES; cycle++) {
        if (TagFSSettle() != OK)
            return false;
        if (StandSealResting() == 0)
            return TagFSSettle() == OK && StandSealResting() == 0;
    }
    return false;
}

/* The volume's medium arriving again, attended until the mount road has
 * nothing left to do — mounted or refused — bounded. */
static void StandSealArrive(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_SEAL_MOUNT_MS);
    TagFSAttendArrival();
    while (!TagFSStandMountIdle() && rdtsc() < bound) {
        BoardroomAttendIfPending();
        TagFSServiceIfPending();
        BatonPump(amp_get_core_index());
        cpu_pause();
    }
}

/* Whatever the test left: the plan down, the volume up again if it is not,
 * and the runner counted back in. */
static void StandSealMountClose(void)
{
    BlockFailDisarm();
    if (!tagfs_get_state()->initialized)
        StandSealArrive();
    TagFSStandHoldAgain();
}

/* The block the file's record lies in, false when the map holds none. */
static bool StandSealRecordBlock(uint64_t fid, uint32_t* block)
{
    TagFSState* fs = tagfs_get_state();
    RecordWhere where;
    FileRecord* rec = FileMapRecordHold(fs->files, fid, &where);
    if (!rec)
        return false;
    FileRecordRelease(rec);
    TagFSLockTake(&fs->lock);
    bool on = PoolLinkBlock(where.link, block);
    TagFSLockDrop(&fs->lock);
    return on;
}

static bool StandSealHolds(uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (rec)
        FileRecordRelease(rec);
    return rec != NULL;
}

/*
 * A sector of the newest seal's open block the medium answers it cannot read
 * — what a page torn as it was written reads as, until it is written again —
 * is that tail torn: the mount rolls the block back to the last closed one
 * (decision 5) and the volume MOUNTS, a file whose only record lay in it gone
 * and a file in a closed block there; nothing waits for seat:ready, and the
 * first cycle writes the block anew. Red before: the answer was taken for a
 * refusal, the mount refused, and it waited for a seat:ready that a sector
 * which stays bad never brings.
 */
static TestResult TestStandSealBadOpenBlockRollsBack(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    uint8_t      seat    = tagfs_get_seat();
    uint32_t     epoch   = tagfs_mount_epoch();
    uint32_t     kept_block = 0, lost_block = 0;
    uint64_t     kept    = 0, lost = 0;
    bool         let_go  = false, has_other = false, fell_back = true;
    int          seal    = -1;
    VolumeLedger newest, other;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xC1u);
    TEST_ASSERT(StandMakeFile("bad_open_kept", g_test_buffer, TAGFS_BLOCK_SIZE, &kept),
                "a file is made and written");
    TEST_ASSERT(StandSealBare(), "and settled by cycles that close the tail, nothing left resting");
    TEST_ASSERT_EQ(tagfs_create_file("bad_open_lost", NULL, 0, &lost), 0,
                   "a second file is made, holding no block");
    TEST_ASSERT_OK(TagFSSettleAsCadence(), "and settled by a cycle the cadence begins");
    TEST_ASSERT(StandSealCopies(&newest, &other, &has_other) && StandSealOf(&newest),
                "the newest copy of the Ledger carries a seal");
    TEST_ASSERT(StandSealRecordBlock(kept, &kept_block) && kept_block != newest.mark.pool_to_block,
                "the first file's record lies in a block the seal counts closed");
    TEST_ASSERT(StandSealRecordBlock(lost, &lost_block) && lost_block == newest.mark.pool_to_block,
                "the second's, its only one, in the seal's open block");
    TEST_ASSERT_EQ(StandSealResting(), 0, "and nothing rests in Fallow");

    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BlockFailArmBadSectors(seat, tagfs_block_to_sector(lost_block), TAGFS_BLOCK_SECTORS);
    StandSealArrive();
    TEST_ASSERT(BlockFailFired(), "the mount met a sector of the open block the medium cannot read");
    TestEvidence("reads of the open block the medium answered so:", BlockFailSeen());
    TEST_ASSERT_EQ(BlockFailSeen(), 2,
                   "in the walk that held the seals, and in the one that rolled the block back");
    TEST_ASSERT(fs->initialized, "and the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSStandMountOwed(), "nothing waits for seat:ready");
    TEST_ASSERT(StandSealHolds(kept), "the file in a closed block is there");
    TEST_ASSERT(!StandSealHolds(lost), "the file whose only record lay in the torn block is gone");

    TEST_ASSERT_OK(TagFSSettle(), "the first cycle settles");
    BlockFailDisarm();
    TEST_ASSERT(StandSealCopies(&newest, &other, &has_other), "the Ledger reads");
    TEST_ASSERT_EQ(StandSealJudge(&newest, &other, has_other, &seal, &fell_back, NULL),
                   POOL_STOOD_SEAL, "and the block it wrote anew bears its seal out");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest");
out:
    if (let_go)
        StandSealMountClose();
    if (kept && fs->initialized) {
        (void)tagfs_delete_file(kept, INK_UNWARRANTED);
        (void)TagFSSettle();
    }
    return verdict;
}

/*
 * A sector of a block both seals count closed the medium answers it cannot
 * read: no kernel with a seal writes a closed block again, so it is rot, and
 * loud. Neither seal is borne out, the chain is read as a kernel before the
 * seal reads it, meets the block again, and the mount is refused for what the
 * volume holds — it waits for no seat:ready, since the medium answered every
 * read; its next arrival is asked. Red before: the answer was taken for a
 * refusal, and the mount waited for a seat:ready.
 */
static TestResult TestStandSealBadClosedBlockIsRot(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    uint8_t      seat    = tagfs_get_seat();
    uint32_t     epoch   = tagfs_mount_epoch();
    uint32_t     bad     = 0;
    bool         let_go  = false, has_other = false;
    VolumeLedger newest, other;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSealBare(), "every change is on the medium, and nothing rests");
    TEST_ASSERT(StandSealCopies(&newest, &other, &has_other) && has_other &&
                StandSealOf(&newest) && StandSealOf(&other), "both copies of the Ledger carry a seal");
    TEST_ASSERT_EQ(newest.mark.chain_first, other.mark.chain_first,
                   "and both begin the chain at one block");
    bad = newest.mark.chain_first;
    TEST_ASSERT(bad != newest.mark.pool_to_block && bad != other.mark.pool_to_block,
                "a block both seals count closed");

    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BlockFailArmBadSectors(seat, tagfs_block_to_sector(bad), TAGFS_BLOCK_SECTORS);
    StandSealArrive();
    TEST_ASSERT(BlockFailFired(), "the mount met a sector of the closed block the medium cannot read");
    TestEvidence("reads of the closed block the medium answered so:", BlockFailSeen());
    TEST_ASSERT_EQ(BlockFailSeen(), 2,
                   "in the walk that held the seals, and in the walk of a kernel before the seal");
    TEST_ASSERT(!fs->initialized, "the volume is not mounted");
    TEST_ASSERT(!TagFSStandMountOwed(), "and nothing waits for seat:ready: the medium answered");

    BlockFailDisarm();
    StandSealArrive();
    TEST_ASSERT(fs->initialized, "the medium's next arrival, the sector readable, MOUNTS the volume");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
out:
    if (let_go)
        StandSealMountClose();
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_seal_tests[] = {
    { .name = "stand_seal_borne",               .func = TestStandSealBorne,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_stitch_coined" },
    { .name = "stand_seal_cut_keeps_the_seal",  .func = TestStandSealCutKeepsTheSeal,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_ribbon_torn_tail" },
    { .name = "stand_seal_lie_falls_back",      .func = TestStandSealLieFallsBack,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_stitch_medium_lied" },
    { .name = "stand_seal_rollback_stands_on_the_newest",
      .func = TestStandSealRollbackStandsOnTheNewest, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_away_newer_slot" },
    { .name = "stand_seal_torn_table_stands",   .func = TestStandSealTornTableStands,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_binder_slot_torn" },
    { .name = "stand_seal_fid_boundary_derived", .func = TestStandSealFidBoundaryDerived,
      .reach = TEST_READS_ONLY },
    /* Last: each takes the volume down and mounts it again. */
    { .name = "stand_seal_bad_open_block_rolls_back", .func = TestStandSealBadOpenBlockRollsBack,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a sector of the open block the medium cannot read refuses the mount, "
                       "waiting for a seat:ready a bad sector never brings",
      .format = DEED_FORMAT_V1, .analog = "stand_errata_rot_late" },
    { .name = "stand_seal_bad_closed_block_is_rot", .func = TestStandSealBadClosedBlockIsRot,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a sector of a closed block the medium cannot read is taken for a "
                       "refusal, and the mount waits for a seat:ready",
      .format = DEED_FORMAT_V1, .analog = "stand_ribbon_closed_damaged" },
};

const TestSuite g_stand_seal_suite = {
    .tests = g_stand_seal_tests, .count = ARRAY_SIZE(g_stand_seal_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
