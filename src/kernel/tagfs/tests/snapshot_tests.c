#include "tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../disk_book/disk_book.h"

// ============================================================================
// Snapshots and copy-on-write — two suites, one subsystem
//
// ‼ Deleting a snapshot GIVES BACK every block it froze (cow.c,
// TagFS_SnapshotDelete). A test that makes a redirect has to leave the volume
// in the state the write path would have: the frozen block the snapshot's,
// the copy the file's. Freeing "what was allocated" on top of that gives the
// frozen block back a second time — a give the block map refuses and says
// (block_map.h), and one the test must not make.
// ============================================================================

static uint32_t s_listed[TAGFS_MAX_SNAPSHOTS];
static uint8_t  s_cow_block[TAGFS_BLOCK_SIZE];

static bool SnapshotIsListed(uint32_t snapshot_id)
{
    uint32_t count = 0;
    if (TagFS_SnapshotList(s_listed, TAGFS_MAX_SNAPSHOTS, &count) != OK)
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (s_listed[i] == snapshot_id)
            return true;
    return false;
}

/* A snapshot of the whole volume, taken, listed, deleted and gone from the
 * list. ERR_ALREADY_EXISTS hands back the id of a snapshot somebody else took
 * under this name, and that one is not this test's to delete. */
static TestResult test_snapshot_create(void)
{
    TestResult verdict     = TEST_PASS;
    uint32_t   snapshot_id = 0;

    error_t made = TagFS_SnapshotCreate("test_snap", 0, &snapshot_id);
    if (made != OK)
        snapshot_id = 0;
    TEST_ASSERT_OK(made, "TagFS_SnapshotCreate should succeed");
    TEST_ASSERT(snapshot_id > 0, "Snapshot ID should be > 0");
    TEST_ASSERT(SnapshotIsListed(snapshot_id), "the list names the snapshot just taken");

    uint32_t taken = snapshot_id;
    snapshot_id = 0;
    TEST_ASSERT_OK(TagFS_SnapshotDelete(taken), "the snapshot can be deleted");
    TEST_ASSERT(!SnapshotIsListed(taken), "and the list no longer names it");
out:
    if (snapshot_id)
        TagFS_SnapshotDelete(snapshot_id);
    return verdict;
}

static const TestCase g_snapshot_tests[] = {
    { .name = "snapshot_create", .func = test_snapshot_create, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_snapshot_suite = {
    .tests = g_snapshot_tests, .count = ARRAY_SIZE(g_snapshot_tests),
};

// ----------------------------------------------------------------------------
// CoW
// ----------------------------------------------------------------------------

static TestResult test_cow_snapshot_create(void)
{
    TestResult verdict     = TEST_PASS;
    uint32_t   snapshot_id = 0;

    error_t made = TagFS_SnapshotCreate("test-snapshot", 0, &snapshot_id);
    if (made != OK)
        snapshot_id = 0;
    TEST_ASSERT_OK(made, "TagFS_SnapshotCreate should succeed");
    TEST_ASSERT(snapshot_id > 0, "Snapshot ID should be > 0");
out:
    if (snapshot_id)
        TagFS_SnapshotDelete(snapshot_id);
    return verdict;
}

/*
 * The half of a copy-on-write that the write path does after
 * TagFS_CowAfterWrite (tagfs.c, tagfs_write_inside): the file's extent moves
 * to the copy, and the metadata saying so is committed by writing the copy's
 * content through the handle. Only once the snapshot is gone — while it
 * stands, the write would be copied for it again, and an auto-snapshot taken
 * besides.
 */
static bool CowFileTakesCopy(uint32_t file_id, uint32_t copy, const uint8_t* content)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return false;
    bool taken = handle->extent_count > 0 && handle->extents &&
                 handle->extents[0].block_count == 1;
    if (taken) {
        handle->extents[0].start_block = copy;
        taken = tagfs_write(handle, content, TAGFS_BLOCK_SIZE) == TAGFS_BLOCK_SIZE;
    }
    tagfs_close(handle);
    return taken;
}

/*
 * CR-12. The copy is taken of the file's OWN first block, with a snapshot of
 * the file standing, and the redirect is looked for where it is kept.
 *
 * This used to call TagFS_CowBeforeWrite on the literal block 1 — the head of
 * the file table, not a block of the file — with no snapshot standing, pass
 * whether or not the call worked, and never give the copy back: one orphaned
 * copy of the table head on every VOLUME_TESTS boot, which put every "no
 * orphan" oracle out of reach on any image that had run the tests.
 */
static TestResult test_cow_before_after_write(void)
{
    TestResult    verdict    = TEST_PASS;
    uint32_t      file_id    = 0;
    uint32_t      snap_id    = 0;
    uint32_t      copy       = 0;
    uint32_t      frozen     = 0;
    uint16_t      tag_ids[1] = {1};
    bool          have_meta  = false;
    TagFSMetadata meta;
    CowSnapshot   info;

    if (TagFS_CowIsActive(0))
        return test_steps_aside("a snapshot of the whole volume is standing, so "
                                "every write would be copied for it and this "
                                "test's redirect could not be told apart");

    /* Two blocks of the file as it was, each its own bytes. */
    TestFillAt(g_test_buffer, 2 * TAGFS_BLOCK_SIZE, 0xC0, 0);
    TEST_ASSERT_EQ(tagfs_create_file("cow-test", tag_ids, 1, &file_id), 0,
                   "tagfs_create_file should succeed");
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_buffer, 2 * TAGFS_BLOCK_SIZE),
                   2 * TAGFS_BLOCK_SIZE, "two blocks are written");

    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "the file's metadata reads");
    have_meta = true;
    uint32_t blocks = 0;
    for (uint16_t e = 0; e < meta.extent_count && meta.extents; e++)
        blocks += meta.extents[e].block_count;
    TEST_ASSERT_EQ(blocks, 2, "the file holds two data blocks");
    TEST_ASSERT_EQ(meta.extents[0].block_count, 1,
                   "its first block is an extent of its own — the only kind the "
                   "write path redirects whole");
    frozen = meta.extents[0].start_block;
    tagfs_metadata_free(&meta);
    have_meta = false;

    error_t made = TagFS_SnapshotCreate("cow-before-after", file_id, &snap_id);
    if (made != OK)
        snap_id = 0;
    TEST_ASSERT_OK(made, "a snapshot of the file is taken");

    TEST_ASSERT_OK(TagFS_CowBeforeWrite(file_id, frozen, &copy),
                   "CowBeforeWrite copies the file's own first block");
    TEST_ASSERT(copy != 0 && copy != frozen, "into a block of its own");
    TEST_ASSERT_OK(tagfs_read_block(copy, s_cow_block), "the copy reads");
    TEST_ASSERT_EQ(TestFirstDifference(s_cow_block, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the copy holds the block as it was");

    TEST_ASSERT_OK(TagFS_CowAfterWrite(file_id, frozen, copy),
                   "CowAfterWrite records the redirect");
    TEST_ASSERT_OK(TagFS_SnapshotInfo(snap_id, &info), "the snapshot is standing");
    TEST_ASSERT_EQ(info.redirect_count, 1, "and holds one redirect");
    TEST_ASSERT_EQ(info.redirects[0].old_block, frozen, "frozen at the file's own block");
    TEST_ASSERT_EQ(info.redirects[0].new_block, copy, "with the file sent on to the copy");

    /* The write the redirect sends to the copy. */
    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0xC1, 0);
    TEST_ASSERT_OK(tagfs_write_block(copy, g_test_output), "the new bytes land in the copy");
    TEST_ASSERT_OK(tagfs_read_block(frozen, s_cow_block), "the frozen block reads");
    TEST_ASSERT_EQ(TestFirstDifference(s_cow_block, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and still holds the old bytes");
    TEST_ASSERT_OK(tagfs_read_block(copy, s_cow_block), "the copy reads again");
    TEST_ASSERT_EQ(TestFirstDifference(s_cow_block, g_test_output, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and holds the new bytes");

out:
    if (have_meta)
        tagfs_metadata_free(&meta);

    if (snap_id) {
        bool froze = TagFS_SnapshotInfo(snap_id, &info) == OK && info.redirect_count > 0;
        TagFS_SnapshotDelete(snap_id);      /* gives the frozen block back */
        if (froze) {
            if (!CowFileTakesCopy(file_id, copy, g_test_output)) {
                /* The file still names the block its snapshot just gave back.
                 * Deleting it would give that block back a second time, so it
                 * stays on the volume, and the runner's count says so. */
                verdict = TestFailedAt(__FILE__, __LINE__,
                                       "the file could not take the copy and still "
                                       "names the block its snapshot gave back — "
                                       "it is left on the volume (file, copy)",
                                       file_id, copy);
                tagfs_free_blocks(copy, 1);
                file_id = 0;
            }
            copy = 0;
        }
    }
    if (copy)
        tagfs_free_blocks(copy, 1);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/* The id of the standing snapshot called `name`, or 0. */
static uint32_t CowSnapshotNamed(const char* name)
{
    uint32_t    count = 0;
    CowSnapshot info;

    if (TagFS_SnapshotList(s_listed, TAGFS_MAX_SNAPSHOTS, &count) != OK)
        return 0;
    for (uint32_t i = 0; i < count; i++)
        if (TagFS_SnapshotInfo(s_listed[i], &info) == OK && strcmp(info.name, name) == 0)
            return s_listed[i];
    return 0;
}

/*
 * W-COW. A file whose data is ONE extent three blocks long, a snapshot of it,
 * and a write of its first block alone. The write path copies that block for
 * the snapshot and moves the extent's start onto the copy (tagfs.c,
 * tagfs_write_inside) — the whole extent, three blocks long, so the file's
 * second and third blocks now read the two blocks after the copy, which it
 * never held. The right answer leaves both of them as they were, and the
 * snapshot's frozen block too; the evidence counts every one of those three
 * that changed.
 *
 * The extent is laid down by gathering three one-block extents into one
 * (tagfs_defrag_file): the write path gives every 4 KiB an extent of its own,
 * so that is how a longer one arises on this volume. Its three blocks are then
 * written over in place with bytes of this file's own (the stream names the
 * file id, which no earlier boot had), so no other block on the volume holds
 * what they hold: not the three the gathering let go of, which first-fit hands
 * out first and still carry the first bytes, and not a block this test left
 * free on an earlier boot. Only the file's own blocks can read back right.
 *
 * ‼ THE FILE THEN NAMES TWO BLOCKS IT DOES NOT OWN, and deleting it as it
 * stands would hand them to the allocator whoever holds them. So once the
 * snapshots are gone its record is cut back to the copy alone, the two blocks
 * the extent was moved off are given back by hand, and only then is the file
 * deleted. The frozen block is NOT given back here: deleting the snapshot
 * already did (cow.c, TagFS_SnapshotDelete), and a second time is the double
 * free this file's header warns of — which is also why a file whose redirect
 * was recorded and whose record never moved onto the copy is not deleted at
 * all, only said. The write also took an auto-snapshot of the file (tagfs.c,
 * tagfs_auto_snapshot_before_write), and that goes too.
 */
#define COW_SPAN_BLOCKS 3u

/* The record cut back to its first block, the copy, through a handle whose
 * extent says so — the same road CowFileTakesCopy takes, with no snapshot
 * standing, so nothing is copied for one. */
static bool CowFileCutToTheCopy(uint32_t file_id, uint32_t copy, const uint8_t* content)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return false;
    bool cut = handle->extent_count == 1 && handle->extents &&
               handle->extents[0].start_block == copy;
    if (cut) {
        handle->extents[0].block_count = 1;
        handle->file_size = 0;
        cut = tagfs_write(handle, content, TAGFS_BLOCK_SIZE) == TAGFS_BLOCK_SIZE;
    }
    tagfs_close(handle);
    return cut;
}

static TestResult TestCowMovesOnlyTheBlockWritten(void)
{
    TestResult    verdict   = TEST_PASS;
    uint32_t      file_id   = 0;
    uint32_t      snap_id   = 0;
    uint32_t      start     = 0;     /* the extent's first block: frozen by the snapshot */
    uint32_t      copy      = 0;     /* where the write sent the first block */
    bool          have_meta = false;
    uint8_t*      was       = g_test_buffer;                                /* 3 blocks */
    uint8_t*      now       = g_test_buffer + COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE;
    char          auto_name[TAGFS_SNAPSHOT_NAME_LEN];
    TagFSMetadata meta;
    CowSnapshot   info;

    if (TagFS_CowIsActive(0))
        return test_steps_aside("a snapshot of the whole volume is standing, so "
                                "every write would be copied for it and this "
                                "test's redirect could not be told apart");

    TEST_ASSERT_EQ(tagfs_create_file("cow-test", NULL, 0, &file_id), 0, "a file is created");
    TestFillAt(g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, 0xC1DE0000u | file_id, 0);
    TestFillAt(was, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, 0xC2DE0000u | file_id, 0);
    TestFillAt(now, TAGFS_BLOCK_SIZE, 0xC3DE0000u | file_id, 0);
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "three blocks are written");
    TEST_ASSERT_EQ(tagfs_defrag_file(file_id, 0), 0, "and gathered into one extent");
    TEST_ASSERT_EQ(TestFileWrite(file_id, was, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "and written over");

    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "the file's metadata reads");
    have_meta = true;
    TEST_ASSERT_EQ(meta.extent_count, 1, "the file is one extent, written over in place");
    TEST_ASSERT_EQ(meta.extents[0].block_count, COW_SPAN_BLOCKS, "three blocks long");
    start = meta.extents[0].start_block;
    tagfs_metadata_free(&meta);
    have_meta = false;

    error_t made = TagFS_SnapshotCreate("cow-one-block", file_id, &snap_id);
    if (made != OK)
        snap_id = 0;        /* ERR_ALREADY_EXISTS names somebody else's snapshot */
    TEST_ASSERT_OK(made, "a snapshot of the file is taken");

    /* The copy is learnt before the write's answer is judged: a write that
     * answers short may still have recorded its redirect, and the way out
     * has to know. */
    int     wrote    = TestFileWrite(file_id, now, TAGFS_BLOCK_SIZE);
    error_t standing = TagFS_SnapshotInfo(snap_id, &info);
    if (standing == OK && info.redirect_count == 1 && info.redirects[0].old_block == start)
        copy = info.redirects[0].new_block;
    TEST_ASSERT_EQ(wrote, TAGFS_BLOCK_SIZE, "the first block alone is written");
    TEST_ASSERT_OK(standing, "the snapshot is standing");

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "the file reads back whole");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, now, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "its first block holds the write");
    uint32_t changed = 0;
    for (uint32_t k = 1; k < COW_SPAN_BLOCKS; k++)
        changed += TestFirstDifference(g_test_output + k * TAGFS_BLOCK_SIZE,
                                       was + k * TAGFS_BLOCK_SIZE,
                                       TAGFS_BLOCK_SIZE) != TAGFS_BLOCK_SIZE;
    TEST_ASSERT_OK(tagfs_read_block(start, s_cow_block), "the frozen block reads");
    changed += TestFirstDifference(s_cow_block, was, TAGFS_BLOCK_SIZE) != TAGFS_BLOCK_SIZE;
    TestEvidence("blocks the write should have left as they were that changed, in "
                 "the file or its snapshot", changed);
    if (changed)
        verdict = TEST_DEFECT;
out:
    if (have_meta)
        tagfs_metadata_free(&meta);
    if (file_id) {
        ksnprintf(auto_name, sizeof(auto_name), "auto-snap-%u", file_id);
        uint32_t auto_id = CowSnapshotNamed(auto_name);
        if (auto_id && auto_id != snap_id)
            TagFS_SnapshotDelete(auto_id);
    }
    if (snap_id)
        TagFS_SnapshotDelete(snap_id);      /* gives the frozen block back */

    /* Where the record's one extent starts now says what is still owed. */
    uint32_t first = 0;
    uint32_t span  = 0;
    if (copy && file_id && tagfs_get_metadata(file_id, &meta) == 0) {
        if (meta.extent_count == 1 && meta.extents) {
            first = meta.extents[0].start_block;
            span  = meta.extents[0].block_count;
        }
        tagfs_metadata_free(&meta);
    }
    if (copy && first == copy && span == COW_SPAN_BLOCKS) {
        tagfs_free_blocks(start + 1, COW_SPAN_BLOCKS - 1);
        if (!CowFileCutToTheCopy(file_id, copy, now)) {
            /* It still names two blocks it never held, and deleting it would
             * give them to the allocator — so it stays, and the runner's count
             * says so. */
            verdict = TestFailedAt(__FILE__, __LINE__,
                                   "the file names blocks it never held and could not "
                                   "be cut back to its copy — it is left on the volume "
                                   "(file, copy)", file_id, copy);
            file_id = 0;
        }
    } else if (copy && first == start) {
        /* The snapshot has just given back the block the record still names;
         * deleting the file would give it back again. The copy is named by
         * nothing now, and goes back once. */
        verdict = TestFailedAt(__FILE__, __LINE__,
                               "the write recorded its redirect and the file never moved "
                               "onto the copy, so it names the block its snapshot gave "
                               "back — it is left on the volume (file, copy)", file_id, copy);
        tagfs_free_blocks(copy, 1);
        file_id = 0;
    }
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/*
 * CoW redirect durability — the snapshot's frozen view survives a reboot.
 *
 * Reproduces the post-reboot state (a snapshot restored from the manifest with
 * no redirects in memory) and runs the real mount-time DiskBook replay, which
 * has to bring the redirect back from the durable log.
 */
#define COW_REBOOT_SNAPSHOT_ID 0x7eb007u   /* "reboot"; the allocator never reaches it */

static TestResult test_cow_redirect_reboot_survival(void)
{
    TestResult  verdict  = TEST_PASS;
    uint32_t    oldb     = 0;
    uint32_t    newb     = 0;
    bool        injected = false;
    CowSnapshot snap;
    CowSnapshot info;

    if (!DiskBookIsInitialized())
        return test_steps_aside("the DiskBook log is not open, and the redirect "
                                "this test looks for is only ever restored from "
                                "it");
    if (TagFS_SnapshotInfo(COW_REBOOT_SNAPSHOT_ID, &info) == OK)
        return test_steps_aside("a snapshot with this test's id is already "
                                "standing — left by a run that did not finish — "
                                "and deleting it would give back blocks this run "
                                "never froze");
    if (tagfs_alloc_blocks(1, &oldb) != 0)
        return test_steps_aside("the volume would not hand out the block a "
                                "redirect has to point away from");
    if (tagfs_alloc_blocks(1, &newb) != 0) {
        newb = 0;
        verdict = test_steps_aside("the volume would not hand out the block a "
                                   "redirect has to point at");
        goto out;
    }

    /* Present in memory with an empty redirect list, exactly as the mount's
     * manifest restore leaves it: redirects are not in the manifest, they live
     * only in the DiskBook log. This IS the post-reboot state before replay. */
    memset(&snap, 0, sizeof(snap));
    snap.snapshot_id = COW_REBOOT_SNAPSHOT_ID;
    strncpy(snap.name, "_reboot_surv", TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap.flags = COW_SNAP_READONLY;
    TagFS_CowRestoreSnapshot(&snap);
    injected = TagFS_SnapshotInfo(COW_REBOOT_SNAPSHOT_ID, &info) == OK;
    TEST_ASSERT(injected, "the test snapshot is standing");
    TEST_ASSERT_EQ(info.redirect_count, 0, "and starts with no redirects, as after a reboot");

    // Durably record a redirect, as a pre-reboot CoW write would have.
    TEST_ASSERT_OK(DiskBookLogRedirect(COW_REBOOT_SNAPSHOT_ID, oldb, newb, 0),
                   "DiskBookLogRedirect persists the redirect");

    // Run the real mount-time recovery path — it must restore the redirect.
    TEST_ASSERT_OK(DiskBookValidateAndReplay(), "DiskBook replay succeeds");
    TEST_ASSERT_OK(TagFS_SnapshotInfo(COW_REBOOT_SNAPSHOT_ID, &info), "the snapshot still stands");
    TEST_ASSERT_EQ(info.redirect_count, 1, "replay restored the redirect (it survives a reboot)");
    TEST_ASSERT_EQ(info.redirects[0].old_block, oldb, "frozen where it was logged");
    TEST_ASSERT_EQ(info.redirects[0].new_block, newb, "and sent where it was logged");

out:
    if (injected) {
        bool froze    = TagFS_SnapshotInfo(COW_REBOOT_SNAPSHOT_ID, &info) == OK &&
                        info.redirect_count > 0;
        bool only_ours = !froze ||
                         (info.redirect_count == 1 && info.redirects[0].old_block == oldb);
        if (!only_ours) {
            /* The log gave this id redirects this run never made. Deleting the
             * snapshot would hand their blocks to the allocator — blocks some
             * file may own — so it is left standing with this run's two blocks
             * in it, and said. */
            verdict = TestFailedAt(__FILE__, __LINE__,
                                   "the snapshot came back from the log holding "
                                   "redirects this run never made — left standing "
                                   "(redirects, wanted)",
                                   info.redirect_count, 1);
            oldb = 0;
            newb = 0;
        } else {
            /* Delete gives back the frozen block when replay restored the
             * redirect, and compacts the log either way. */
            TagFS_SnapshotDelete(COW_REBOOT_SNAPSHOT_ID);
            if (froze)
                oldb = 0;
        }
    }
    if (oldb)
        tagfs_free_blocks(oldb, 1);
    if (newb)
        tagfs_free_blocks(newb, 1);
    return verdict;
}

static const TestCase g_cow_tests[] = {
    { .name = "cow_snapshot_create",          .func = test_cow_snapshot_create,          .reach = TEST_WRITES_TO_VOLUME },
    { .name = "cow_before_after_write",       .func = test_cow_before_after_write,       .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "cow_moves_only_the_block_written", .func = TestCowMovesOnlyTheBlockWritten,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.3",
      .caveat_defect = "a write into the first block of a longer extent, under a snapshot, "
                       "moves the whole extent onto a one-block copy, so the file's later "
                       "blocks read blocks it never held" },
    { .name = "cow_redirect_reboot_survival", .func = test_cow_redirect_reboot_survival, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_cow_suite = {
    .tests = g_cow_tests, .count = ARRAY_SIZE(g_cow_tests),
};
