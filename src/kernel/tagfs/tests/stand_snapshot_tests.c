#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../disk_book/disk_book.h"
#include "../mount/ground_census.h"
#include "../../lib/kernel/crypto.h"
#include "op_registry.h"
#include "boxos_decks.h"
#include "boxos_manifest.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The snapshots an earlier kernel left (step 1.9)
//
// A volume a kernel from before format v1's refusal wrote may carry snapshots:
// a manifest the Ledger names, and redirects in the DiskBook. Here one is laid
// down the way that kernel laid it — a manifest in a block of the data run, a
// head and its records in the DiskBook — and read by the functions the mount
// reads it with: the snapshots are named, every request for a new one is
// refused, and the census claims the manifest alone, so the blocks the
// snapshots kept are orphans it names and keeps taken.
//
// The Ledger is never told of the manifest, so a mount that follows a test cut
// short reads no book; every sector of the book the test writes is put back
// all the same, and every block it takes is given back.
// ============================================================================

#define STAND_SNAP_FROZEN   3u    /* blocks the older kernel's snapshots froze */
#define STAND_SNAP_RECORDS  6u    /* redirect records laid in its book */
#define STAND_SNAP_OF_FILE  41u   /* the snapshot of one file */
#define STAND_SNAP_OF_ALL   42u   /* the snapshot of the whole volume */
#define STAND_SNAP_NOBODY   99u   /* a snapshot the manifest does not carry */

/* The book's sectors this test writes over: the head, its copy, and the
 * records — saved in g_test_buffer in that order, and put back from it. */
#define STAND_SNAP_SAVED_SECTORS (2u + STAND_SNAP_RECORDS * DISK_BOOK_SECTORS_PER_ENTRY)

static uint64_t StandSnapBookAt(uint32_t part)
{
    return ((uint64_t)tagfs_get_state()->layout.disk_book_block + part) * TAGFS_BLOCK_SECTORS;
}

/* The book's sectors in the order they are saved: head, copy, records. */
static int StandSnapBookIo(uint8_t* buf, bool write)
{
    int bad = 0;
    for (uint32_t part = 0; part < 2; part++) {
        uint8_t* at = buf + part * TAGFS_SECTOR_SIZE;
        bad |= write ? tagfs_volume_write(StandSnapBookAt(part), 1, at)
                     : tagfs_volume_read(StandSnapBookAt(part), 1, at);
    }
    uint8_t* records = buf + 2u * TAGFS_SECTOR_SIZE;
    uint32_t count   = STAND_SNAP_RECORDS * DISK_BOOK_SECTORS_PER_ENTRY;
    bad |= write ? tagfs_volume_write(StandSnapBookAt(2), count, records)
                 : tagfs_volume_read(StandSnapBookAt(2), count, records);
    return bad;
}

static void StandSnapHead(DiskBookSuperblock* h, uint32_t count, bool whole)
{
    memset(h, 0, sizeof(*h));
    h->magic        = DISK_BOOK_SB_MAGIC;
    h->version      = DISK_BOOK_VERSION;
    h->start_sector = StandSnapBookAt(2);
    h->capacity     = DISK_BOOK_CAPACITY;
    h->count        = count;
    h->crc32        = KCrc32((const uint8_t*)h, sizeof(*h)) ^ (whole ? 0u : 1u);
}

static void StandSnapRecord(DiskBookEntry* e, uint32_t snapshot, uint32_t old_block,
                            uint32_t new_block, bool whole)
{
    memset(e, 0, sizeof(*e));
    e->magic        = DISK_BOOK_MAGIC;
    e->type         = DISK_BOOK_TYPE_REDIRECT;
    e->snapshot_id  = snapshot;
    e->old_block    = old_block;
    e->new_block    = new_block;
    e->record_crc32 = KCrc32((const uint8_t*)e, sizeof(*e)) ^ (whole ? 0u : 1u);
}

/* The book as the older kernel left it, in g_test_output: a head whose first
 * copy is `head_whole` and whose second is `copy_whole`, and six records —
 * each snapshot's, a repeat, one naming a snapshot the manifest does not
 * carry, and one that fails its checksum. */
static int StandSnapBookLay(const uint32_t* frozen, uint32_t live, bool head_whole,
                            bool copy_whole)
{
    DiskBookSuperblock* head = (DiskBookSuperblock*)g_test_output;
    DiskBookSuperblock* copy = (DiskBookSuperblock*)(g_test_output + TAGFS_SECTOR_SIZE);
    DiskBookEntry*      rec  = (DiskBookEntry*)(g_test_output + 2u * TAGFS_SECTOR_SIZE);

    StandSnapHead(head, STAND_SNAP_RECORDS, head_whole);
    StandSnapHead(copy, STAND_SNAP_RECORDS, copy_whole);
    StandSnapRecord(&rec[0], STAND_SNAP_OF_FILE, frozen[0], live, true);
    StandSnapRecord(&rec[1], STAND_SNAP_OF_ALL,  frozen[1], live, true);
    StandSnapRecord(&rec[2], STAND_SNAP_OF_ALL,  frozen[2], live, true);
    StandSnapRecord(&rec[3], STAND_SNAP_OF_ALL,  frozen[1], live, true);
    StandSnapRecord(&rec[4], STAND_SNAP_NOBODY,  frozen[0], live, true);
    StandSnapRecord(&rec[5], STAND_SNAP_OF_ALL,  frozen[0], live, false);
    return StandSnapBookIo(g_test_output, true);
}

/* The manifest as the older kernel left it: one snapshot of the file, one of
 * the whole volume. */
static int StandSnapManifestLay(uint32_t block, uint64_t file_id)
{
    CowManifest* m = (CowManifest*)g_test_output;
    memset(m, 0, sizeof(*m));
    m->magic                     = COW_MANIFEST_MAGIC;
    m->count                     = 2;
    m->entries[0].snapshot_id    = STAND_SNAP_OF_FILE;
    m->entries[0].parent_file_id = (uint32_t)file_id;   /* format v1: the manifest's u32 */
    memcpy(m->entries[0].name, "auto-snap-41", 12);
    m->entries[1].snapshot_id    = STAND_SNAP_OF_ALL;
    memcpy(m->entries[1].name, "nightly", 7);
    return tagfs_write_block(block, m);
}

static bool StandSnapKeeps(const CowSnapshot* s, uint32_t block)
{
    for (uint32_t k = 0; k < s->kept_count; k++)
        if (s->kept[k] == block)
            return true;
    return false;
}

/* What the deck answers one snapshot request. */
static int StandSnapAsk(uint16_t opcode)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_STORAGE, opcode));
    ManifestOp            op;
    OpContext             ctx;
    if (!reg || !reg->handler)
        return -1;
    memset(&op, 0, sizeof(op));
    memset(&ctx, 0, sizeof(ctx));
    op.op_kind = OP_KIND(DECK_STORAGE, opcode);
    return reg->handler(&op, NULL, 0, &ctx);
}

/*
 * A volume an older kernel left with two snapshots — one of a file, one of the
 * whole volume — whose redirects froze three blocks:
 *
 *   read as the mount reads it, both are named, with the blocks each kept
 *   (a repeated redirect counted once, one naming a snapshot the manifest does
 *   not carry passed over, one that fails its checksum skipped); a book whose
 *   head is torn is read from its copy, and one with both torn names the
 *   snapshots without their blocks;
 *
 *   every snapshot request the storage deck knows is answered ERR_UNSUPPORTED;
 *
 *   the census's owner claims the manifest and no block a snapshot kept, nor
 *   the block the file moved onto — so the census names the frozen blocks
 *   orphans — and none of them is given back.
 */
static TestResult TestStandOldSnapshotsHeld(void)
{
    TestResult verdict = TEST_PASS;
    CowReading reading;
    uint32_t   run = 0, live = 0;
    uint64_t   file_id = 0;
    uint64_t   taken = 0, block = 0;
    uint32_t   frozen[STAND_SNAP_FROZEN];
    uint8_t    file_bytes[64];
    bool       saved = false, census = false;
    uint64_t   claimed = 0;
    const uint32_t book = tagfs_get_state()->layout.disk_book_block;
    static const uint16_t asks[] = {
        STORAGE_SNAP_CREATE, STORAGE_SNAP_DELETE, STORAGE_SNAP_LIST, STORAGE_SNAP_INFO,
    };

    memset(&reading, 0, sizeof(reading));
    fill_pattern(file_bytes, sizeof(file_bytes), 0x5A);
    TEST_ASSERT(StandMakeFile("snapold", file_bytes, sizeof(file_bytes), &file_id),
                "the file a snapshot was taken of is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "and its block is known");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(1u + STAND_SNAP_FROZEN, &taken), 0,
                   "blocks for the manifest and the frozen blocks are taken");
    /* The book and the manifest are format v1's, and name blocks in u32. */
    live = (uint32_t)block;
    run  = (uint32_t)taken;
    for (uint32_t i = 0; i < STAND_SNAP_FROZEN; i++)
        frozen[i] = run + 1u + i;

    TEST_ASSERT(StandSnapBookIo(g_test_buffer, false) == 0, "the book's sectors are saved");
    saved = true;
    TEST_ASSERT(StandSnapManifestLay(run, file_id) == 0, "the manifest is laid down");
    TEST_ASSERT(StandSnapBookLay(frozen, live, true, true) == 0, "and the book");

    CowReadStanding(run, book, &reading);
    CowSayStanding(&reading);
    TEST_ASSERT_EQ(reading.count, 2, "the mount names both snapshots");
    TEST_ASSERT_OK(reading.book, "its book is replayed");
    TEST_ASSERT_EQ(reading.snapshots[0].snapshot_id, STAND_SNAP_OF_FILE, "the file's first");
    TEST_ASSERT(strcmp(reading.snapshots[0].name, "auto-snap-41") == 0, "by its name");
    TEST_ASSERT_EQ(reading.snapshots[0].parent_file_id, file_id, "of its file");
    TEST_ASSERT_EQ(reading.snapshots[0].kept_count, 1, "keeping one block");
    TEST_ASSERT(StandSnapKeeps(&reading.snapshots[0], frozen[0]), "the one it froze");
    TEST_ASSERT_EQ(reading.snapshots[1].snapshot_id, STAND_SNAP_OF_ALL, "the volume's second");
    TEST_ASSERT(strcmp(reading.snapshots[1].name, "nightly") == 0, "by its name");
    TEST_ASSERT_EQ(reading.snapshots[1].parent_file_id, 0, "of the whole volume");
    TEST_ASSERT_EQ(reading.snapshots[1].kept_count, 2, "keeping two, the repeat counted once");
    TEST_ASSERT(StandSnapKeeps(&reading.snapshots[1], frozen[1]) &&
                StandSnapKeeps(&reading.snapshots[1], frozen[2]), "the two it froze");
    TEST_ASSERT_EQ(reading.stray, 1, "the redirect of a snapshot not carried is passed over");
    TEST_ASSERT(!reading.short_of_memory, "and nothing ran out of memory");
    CowReadingDrop(&reading);

    TEST_ASSERT(StandSnapBookLay(frozen, live, false, true) == 0, "the head is torn");
    CowReadStanding(run, book, &reading);
    TEST_ASSERT_OK(reading.book, "the book is read from the head's copy");
    TEST_ASSERT_EQ(reading.snapshots[0].kept_count + reading.snapshots[1].kept_count, 3,
                   "and every frozen block is named");
    CowReadingDrop(&reading);

    TEST_ASSERT(StandSnapBookLay(frozen, live, false, false) == 0, "both copies are torn");
    CowReadStanding(run, book, &reading);
    TEST_ASSERT_EQ(reading.count, 2, "the snapshots are still named");
    TEST_ASSERT_EQ(reading.book, ERR_TAGFS_CORRUPTED, "and the book is said not to be taken");
    TEST_ASSERT_EQ(reading.snapshots[0].kept_count + reading.snapshots[1].kept_count, 0,
                   "so no block is counted");
    CowReadingDrop(&reading);

    for (uint32_t i = 0; i < ARRAY_SIZE(asks); i++)
        TEST_ASSERT_EQ(StandSnapAsk(asks[i]), ERR_UNSUPPORTED,
                       "the storage deck refuses a snapshot request");

    GroundCensusBegin();
    census = true;
    CowClaimGround(run);
    TEST_ASSERT_EQ(GroundClaimOf(run), GROUND_MANIFEST, "the census's owner claims the manifest");
    for (uint32_t i = 0; i < STAND_SNAP_FROZEN; i++)
        claimed += GroundClaimOf(frozen[i]) != GROUND_OWNER_KINDS;
    claimed += GroundClaimOf(live) != GROUND_OWNER_KINDS;
    TestEvidence("blocks the snapshots kept, and their file's, that copy-on-write claims", claimed);
    TEST_ASSERT_EQ(claimed, 0, "and no block a snapshot kept, nor its file's");
    for (uint32_t i = 0; i < STAND_SNAP_FROZEN; i++)
        TEST_ASSERT(StandBlockInUse(frozen[i]), "a frozen block stays taken");
out:
    if (census)
        GroundCensusEnd();
    CowReadingDrop(&reading);
    if (saved && StandSnapBookIo(g_test_buffer, true) != 0 && verdict == TEST_PASS)
        verdict = TestFailedAt(__FILE__, __LINE__, "the book's sectors are put back", 1, 0);
    if (run)
        tagfs_free_blocks(run, 1u + STAND_SNAP_FROZEN);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static const TestCase g_stand_snapshot_tests[] = {
    { .name  = "stand_old_snapshots_held", .func = TestStandOldSnapshotsHeld,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_snapshot_suite = {
    .tests = g_stand_snapshot_tests, .count = ARRAY_SIZE(g_stand_snapshot_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
