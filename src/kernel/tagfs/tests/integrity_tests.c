#include "tests.h"
#include "../tagfs.h"
#include "../integrity/integrity.h"
#include "touch.h"

// ============================================================================
// Data integrity — verify-on-read
//
// ‼ The block is WRITTEN, not merely hashed. IntegrityUpdate over bytes that
// never reach the medium leaves the map holding a digest the block does not
// have; once the block is freed, whoever is handed it next and reads it before
// writing it is told of bit rot that never happened — counted, named and
// published on the "integrity" tag. tagfs_write_block records the digest of
// exactly what landed, so the map and the medium agree after every test here.
// ============================================================================

static TestResult test_integrity_detects_mismatch(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   blk     = 0;

    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so "
                                "there is no digest map to write into or catch "
                                "a flipped bit with");
    if (tagfs_alloc_blocks(1, &blk) != 0)
        return test_steps_aside("the volume would not hand out a block to record "
                                "a digest over");

    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++) g_test_buffer[i] = (uint8_t)(i * 7 + 1);
    TEST_ASSERT_OK(tagfs_write_block(blk, g_test_buffer),
                   "the block takes its content, and the map its digest");
    TEST_ASSERT(IntegrityVerify(blk, g_test_buffer), "verify matches the recorded digest");

    uint32_t before = IntegrityErrorCount();
    g_test_buffer[100] ^= 0xFF;                       // a flipped byte, as a read might bring it
    TEST_ASSERT(!IntegrityVerify(blk, g_test_buffer), "verify detects a 1-byte mismatch");
    TEST_ASSERT_EQ(IntegrityErrorCount(), before + 1, "bit-rot counter incremented exactly once");
out:
    if (blk)
        tagfs_free_blocks(blk, 1);
    return verdict;
}

/*
 * Integrity-map reboot survival: persist, release the in-RAM map, load it back
 * from disk, and a digest written before the cycle still verifies — and still
 * catches corruption. The map's own reboot-recovery path, exercised in-kernel.
 *
 * Verify-on-read is DOWN between the shutdown and the re-init, for every
 * block of the volume, so the re-init is made on every path out of here.
 */
static TestResult test_integrity_persist_reload(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   blk     = 0;
    bool       down    = false;

    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so "
                                "there is no digest map to write into or catch "
                                "a flipped bit with");
    if (tagfs_alloc_blocks(1, &blk) != 0)
        return test_steps_aside("the volume would not hand out a block to record "
                                "a digest over");

    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++) g_test_buffer[i] = (uint8_t)(i * 11 + 3);
    TEST_ASSERT_OK(tagfs_write_block(blk, g_test_buffer),
                   "the block takes its content, and the map its digest");
    TEST_ASSERT_OK(IntegrityFlush(), "the map is persisted to disk");

    IntegrityShutdown();                    // drop the in-RAM map (the disk copy remains)
    down = true;
    TEST_ASSERT_OK(IntegrityInit(), "integrity remount (re-init from disk) succeeds");
    down = false;

    TEST_ASSERT(IntegrityVerify(blk, g_test_buffer), "digest survived persist + reload (reboot)");
    g_test_buffer[50] ^= 0xFF;
    TEST_ASSERT(!IntegrityVerify(blk, g_test_buffer), "reloaded map still detects corruption");
out:
    if (down) {
        error_t again = IntegrityInit();
        if (again != OK)
            verdict = TestFailedAt(__FILE__, __LINE__,
                                   "verify-on-read could not be brought back, and "
                                   "stays down for the rest of this boot (error, OK)",
                                   again, OK);
    }
    if (blk)
        tagfs_free_blocks(blk, 1);
    return verdict;
}

// ----------------------------------------------------------------------------
// rot65 — is every rotten block reported, or only the first 64?
//
// A caught rot is counted at once (g_errors) and named at once: one Touch on
// the "integrity" tag per block, from inside the verify, the set of blocks
// named this mount growing as rot is found (integrity_rot.c). It used to wait
// in a ring of 64 for a drain in the Settle cycle, and the 65th was dropped
// without a word. The ear is the kernel's own TouchWatch, which is called on
// the publisher's stack inside TouchPublishId (touch.c, touch_watch_deliver),
// so every report has arrived by the time the verify returns. Each block is
// named once: verifying one of them again tells nobody anything new.
// ----------------------------------------------------------------------------

#define ROT_WITNESS_BLOCKS 65u

typedef struct {
    uint32_t first;
    uint64_t heard[2];      /* bit i: a report for block first + i arrived */
    uint32_t reports;       /* reports for the test's blocks, however many per block */
} RotWitness;

static RotWitness s_rot_witness;

/* Records only: it runs inside the publish, with the publisher's locks. */
static void RotWitnessHeard(TouchTag tag_id, const void* payload, uint32_t plen,
                            uint32_t source_pid, void* ctx)
{
    (void)tag_id;
    (void)source_pid;
    RotWitness* witness = (RotWitness*)ctx;
    uint32_t    block;

    if (!payload || plen < sizeof(block))
        return;
    memcpy(&block, payload, sizeof(block));
    if (block < witness->first || block - witness->first >= ROT_WITNESS_BLOCKS)
        return;
    uint32_t i = block - witness->first;
    __atomic_or_fetch(&witness->heard[i / 64], 1ull << (i % 64), __ATOMIC_RELAXED);
    __atomic_add_fetch(&witness->reports, 1u, __ATOMIC_RELAXED);
}

static TestResult TestIntegrityReportsEveryRot(void)
{
    TestResult  verdict     = TEST_PASS;
    uint32_t    first       = 0;
    bool        have_blocks = false;
    TouchWatch* ear         = NULL;

    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so "
                                "no rot can be caught and none reported");
    TouchTag tag = tagfs_tag_lookup("integrity");
    if (tag == TOUCH_TAG_INVALID)
        return test_steps_aside("this volume has no \"integrity\" tag, so a rot "
                                "report has nowhere to be published");
    if (tagfs_alloc_blocks(ROT_WITNESS_BLOCKS, &first) != 0)
        return test_steps_aside("the volume would not hand out 65 blocks in a row "
                                "to rot");
    have_blocks = true;

    memset(&s_rot_witness, 0, sizeof(s_rot_witness));
    s_rot_witness.first = first;
    ear = TouchWatchSet(tag, RotWitnessHeard, &s_rot_witness);
    TEST_ASSERT(ear != NULL, "an ear on the integrity tag");

    uint32_t errors_before = IntegrityErrorCount();
    uint32_t caught        = 0;
    for (uint32_t i = 0; i < ROT_WITNESS_BLOCKS; i++) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x65, (uint64_t)i * TAGFS_BLOCK_SIZE);
        TEST_ASSERT_OK(tagfs_write_block(first + i, g_test_buffer),
                       "every block takes its content, and the map its digest");
        g_test_buffer[i] ^= 0xFF;               // one byte of it rots
        if (!IntegrityVerify(first + i, g_test_buffer))
            caught++;
    }
    TEST_ASSERT_EQ(caught, ROT_WITNESS_BLOCKS, "every rotten block is caught");
    TEST_ASSERT_EQ(IntegrityErrorCount() - errors_before, ROT_WITNESS_BLOCKS,
                   "and counted once");

    uint32_t reported = CountSetBits(s_rot_witness.heard[0]) +
                        CountSetBits(s_rot_witness.heard[1]);
    TestEvidence("of 65 rotten blocks, reported", reported);
    TEST_ASSERT_EQ(reported, ROT_WITNESS_BLOCKS, "every rotten block is reported");
    TEST_ASSERT_EQ(s_rot_witness.reports, ROT_WITNESS_BLOCKS, "each once");

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x65, 0);
    g_test_buffer[0] ^= 0xFF;
    TEST_ASSERT(!IntegrityVerify(first, g_test_buffer), "the first rotten block is caught again");
    TEST_ASSERT_EQ(s_rot_witness.reports, ROT_WITNESS_BLOCKS,
                   "and not reported again: it was named this mount already");
out:
    if (ear)
        TouchWatchClear(ear);
    if (have_blocks)
        tagfs_free_blocks(first, ROT_WITNESS_BLOCKS);
    return verdict;
}

static const TestCase g_integrity_tests[] = {
    { .name = "integrity_detects_mismatch", .func = test_integrity_detects_mismatch, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "integrity_persist_reload",   .func = test_integrity_persist_reload,   .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "integrity_reports_every_rot", .func = TestIntegrityReportsEveryRot,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "the integrity report ring holds 64 — a 65th rotten block "
                       "is caught and never reported" },
};

const TestSuite g_integrity_suite = {
    .tests = g_integrity_tests, .count = ARRAY_SIZE(g_integrity_tests),
};
