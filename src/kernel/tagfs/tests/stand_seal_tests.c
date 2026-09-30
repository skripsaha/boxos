#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../metadata_pool/meta_pool.h"
#include "../mount/mount.h"
#include "../ledger/ledger.h"
#include "../settle/settle.h"
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
 * mount would stand on. */
static PoolStood StandSealJudge(const VolumeLedger* newest, const VolumeLedger* other,
                                bool has_other, int* seal, bool* fell_back)
{
    PoolMountAsk ask = {
        .seal = { &newest->mark, has_other && StandSealOf(other) ? &other->mark : NULL },
        .head = StandPoolHead(),
    };
    return PoolSealsJudge(&ask, seal, fell_back);
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

    PoolStood stood = StandSealJudge(&newest, &other, has_other, &seal, &fell_back);
    TEST_ASSERT_EQ(stood, POOL_STOOD_SEAL, "the mount would stand on a seal");
    TEST_ASSERT_EQ(seal, 0, "the newest one");
    TEST_ASSERT(!fell_back, "without falling back");

    block = kmalloc(TAGFS_BLOCK_SIZE);
    TEST_ASSERT(block, "memory for the registry's block");
    TEST_ASSERT_OK(tagfs_read_block(newest.mark.registry_block, block),
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
    uint32_t     file_id = 0;
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
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back), POOL_STOOD_SEAL,
                   "the mount would stand on the seal the cut left");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest, borne out");

    StandSealRefuseWrite(fs->layout.state_block, VOLUME_LEDGER_COPIES);
    cut = TagFSSettle();
    BlockFailDisarm();
    TEST_ASSERT(BlockFailFired(), "the cycle's Ledger write was refused");
    TEST_ASSERT(cut != OK, "and the cycle cut at its Ledger is refused");
    TEST_ASSERT(StandSealCopies(&after, &other, &has_other), "the Ledger reads");
    TEST_ASSERT_EQ(after.seq, before.seq, "no copy moved past the cut");
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back), POOL_STOOD_SEAL,
                   "the pool the cut cycle wrote past the seal still bears it out");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest, borne out");

    TEST_ASSERT_OK(TagFSSettle(), "with the medium answering, the cycle settles");
    TEST_ASSERT(StandSealCopies(&after, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(after.seq > before.seq, "a newer copy landed");
    TEST_ASSERT(StandSealOf(&after), "sealed");
    TEST_ASSERT_EQ(StandSealJudge(&after, &other, has_other, &seal, &fell_back), POOL_STOOD_SEAL,
                   "and its seal is borne out");
    TEST_ASSERT(seal == 0 && !fell_back, "the newest");
out:
    BlockFailDisarm();
    if (file_id) {
        tagfs_delete_file(file_id);
        (void)TagFSSettle();
    }
    return verdict;
}

/*
 * The medium said a flush was kept and was not: the newest seal names pool
 * records the medium does not hold. The mount falls back to the copy before
 * it (and marks the medium SETTLE_MEDIUM_LIED — PoolMount's fell_back); a seal
 * whose closed part is not borne out falls back the same way; an open block
 * neither seal bears out rolls back to the last closed block (decision 5);
 * and no closed part borne out is the road of a kernel before the seal.
 */
static TestResult TestStandSealLieFallsBack(void)
{
    TestResult   verdict = TEST_PASS;
    VolumeLedger real, other, lied, torn;
    bool         has_other = false, fell_back = false;
    int          seal = -1;

    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle settles");
    TEST_ASSERT(StandSealCopies(&real, &other, &has_other), "the Ledger reads");
    TEST_ASSERT(StandSealOf(&real), "and is sealed");

    /* The newest cycle's open block as the lie leaves it: short of the seal. */
    lied = real;
    lied.seq++;
    lied.mark.settle        = lied.seq;
    lied.mark.pool_to_bytes = real.mark.pool_to_bytes + 42u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &real, true, &seal, &fell_back), POOL_STOOD_SEAL,
                   "an open block short of the newest seal: the mount stands on a seal");
    TEST_ASSERT_EQ(seal, 1, "the one before it");
    TEST_ASSERT(fell_back, "and says it fell back");

    /* Its closed blocks as the lie leaves them. */
    lied = real;
    lied.seq++;
    lied.mark.settle        = lied.seq;
    lied.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &real, true, &seal, &fell_back), POOL_STOOD_SEAL,
                   "closed blocks the newest seal does not find: the mount stands on a seal");
    TEST_ASSERT(seal == 1 && fell_back, "the one before it, falling back");

    /* Neither seal's open block borne out, their closed blocks both: torn. */
    lied = real;
    lied.seq++;
    lied.mark.settle      = lied.seq;
    lied.mark.tail_crc32 ^= 1u;
    torn = real;
    torn.mark.tail_crc32 ^= 2u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &torn, true, &seal, &fell_back),
                   POOL_STOOD_TORN_TAIL, "an open block neither seal finds rolls back");
    TEST_ASSERT(seal == 0 && !fell_back, "under the newest seal, without falling back");

    /* No seal's closed part borne out: the road of a kernel before the seal. */
    lied.mark.closed_crc32 ^= 1u;
    torn.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(StandSealJudge(&lied, &torn, true, &seal, &fell_back),
                   POOL_STOOD_NO_SEAL, "closed blocks no seal finds: the whole chain, unsealed");
    TEST_ASSERT_EQ(seal, -1, "on no seal");
out:
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
    uint32_t        head = 0, torn = 0;
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
    TEST_ASSERT_OK(tagfs_write_block(torn, image), "the second block is garbage");
    memset(image, 0, sizeof(*image));
    image->magic      = TAGFS_FILETBL_MAGIC;
    image->next_block = torn;
    TEST_ASSERT_OK(tagfs_write_block(head, image), "the head links it");

    TEST_ASSERT(FileTableWouldStand(head, true), "under a seal the torn table still mounts");
    TEST_ASSERT(!FileTableWouldStand(head, false), "with no seal it does not");
out:
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
    uint32_t   batch[64];
    uint32_t   from = 1, n, ceiling;

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
        if (batch[n - 1] == 0xFFFFFFFFu)
            break;
        from = batch[n - 1] + 1;
    }
out:
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_seal_tests[] = {
    { .name = "stand_seal_borne",               .func = TestStandSealBorne,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_seal_cut_keeps_the_seal",  .func = TestStandSealCutKeepsTheSeal,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_seal_lie_falls_back",      .func = TestStandSealLieFallsBack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_seal_torn_table_stands",   .func = TestStandSealTornTableStands,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_seal_fid_boundary_derived", .func = TestStandSealFidBoundaryDerived,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_seal_suite = {
    .tests = g_stand_seal_tests, .count = ARRAY_SIZE(g_stand_seal_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
