#include "journal.h"
#include "tagfs.h"
#include "klib.h"
#include "../drivers/disk/ata.h"

static int test_journal_init(void) {
    debug_printf("[JournalTest] Testing journal_init...\n");

    if (journal_init() != 0) {
        debug_printf("[JournalTest] FAIL: journal_init failed\n");
        return -1;
    }

    debug_printf("[JournalTest] PASS: journal_init\n");
    return 0;
}

static int test_journal_metadata_write(void) {
    debug_printf("[JournalTest] Testing metadata write...\n");

    TagFSMetadata saved_meta_1;
    int had_saved_1 = (tagfs_read_metadata(1, &saved_meta_1) == 0);

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(TagFSMetadata));
    meta.magic = TAGFS_METADATA_MAGIC;
    meta.file_id = 1;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 1024;
    meta.start_block = 0;
    meta.block_count = 1;
    strncpy(meta.filename, "test.txt", TAGFS_MAX_FILENAME);

    if (tagfs_write_metadata_journaled(1, &meta) != 0) {
        debug_printf("[JournalTest] FAIL: write_metadata_journaled failed\n");
        if (had_saved_1) tagfs_write_metadata_journaled(1, &saved_meta_1);
        return -1;
    }

    TagFSMetadata read_meta;
    if (tagfs_read_metadata(1, &read_meta) != 0) {
        debug_printf("[JournalTest] FAIL: read_metadata failed\n");
        if (had_saved_1) tagfs_write_metadata_journaled(1, &saved_meta_1);
        return -1;
    }

    if (read_meta.file_id != 1 || read_meta.size != 1024) {
        debug_printf("[JournalTest] FAIL: data mismatch\n");
        if (had_saved_1) tagfs_write_metadata_journaled(1, &saved_meta_1);
        return -1;
    }

    debug_printf("[JournalTest] PASS: metadata write\n");
    if (had_saved_1) {
        tagfs_write_metadata_journaled(1, &saved_meta_1);
    }
    return 0;
}

static int test_journal_superblock_write(void) {
    debug_printf("[JournalTest] Testing superblock write...\n");

    TagFSSuperblock sb;
    if (tagfs_read_superblock(&sb) != 0) {
        debug_printf("[JournalTest] FAIL: read_superblock failed\n");
        return -1;
    }

    uint32_t old_free_blocks = sb.free_blocks;
    sb.free_blocks = 9999;

    if (tagfs_write_superblock_journaled(&sb) != 0) {
        debug_printf("[JournalTest] FAIL: write_superblock_journaled failed\n");
        return -1;
    }

    TagFSSuperblock read_sb;
    if (tagfs_read_superblock(&read_sb) != 0) {
        debug_printf("[JournalTest] FAIL: read_superblock after write failed\n");
        return -1;
    }

    if (read_sb.free_blocks != 9999) {
        debug_printf("[JournalTest] FAIL: superblock data mismatch\n");
        return -1;
    }

    sb.free_blocks = old_free_blocks;
    tagfs_write_superblock_journaled(&sb);

    debug_printf("[JournalTest] PASS: superblock write\n");
    return 0;
}

static int test_journal_replay(void) {
    debug_printf("[JournalTest] Testing journal replay...\n");

    TagFSMetadata saved_meta_2;
    int had_saved_2 = (tagfs_read_metadata(2, &saved_meta_2) == 0);

    uint32_t txn_id;
    if (journal_begin(&txn_id) != 0) {
        debug_printf("[JournalTest] FAIL: journal_begin failed\n");
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(TagFSMetadata));
    meta.magic = TAGFS_METADATA_MAGIC;
    meta.file_id = 2;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 2048;
    strncpy(meta.filename, "replay.txt", TAGFS_MAX_FILENAME);

    if (journal_log_metadata(txn_id, 2, &meta) != 0) {
        debug_printf("[JournalTest] FAIL: journal_log_metadata failed\n");
        journal_abort(txn_id);
        return -1;
    }

    if (journal_commit(txn_id) != 0) {
        debug_printf("[JournalTest] FAIL: journal_commit failed\n");
        journal_abort(txn_id);
        return -1;
    }

    if (journal_replay() != 0) {
        debug_printf("[JournalTest] FAIL: journal_replay failed\n");
        if (had_saved_2) tagfs_write_metadata_journaled(2, &saved_meta_2);
        return -1;
    }

    TagFSMetadata read_meta;
    if (tagfs_read_metadata(2, &read_meta) != 0) {
        debug_printf("[JournalTest] FAIL: read_metadata after replay failed\n");
        if (had_saved_2) tagfs_write_metadata_journaled(2, &saved_meta_2);
        return -1;
    }

    if (read_meta.file_id != 2 || read_meta.size != 2048) {
        debug_printf("[JournalTest] FAIL: replay data mismatch\n");
        if (had_saved_2) tagfs_write_metadata_journaled(2, &saved_meta_2);
        return -1;
    }

    debug_printf("[JournalTest] PASS: journal replay\n");
    if (had_saved_2) {
        tagfs_write_metadata_journaled(2, &saved_meta_2);
    }
    return 0;
}

static int test_journal_abort(void) {
    debug_printf("[JournalTest] Testing journal abort...\n");

    TagFSMetadata original;
    if (tagfs_read_metadata(3, &original) != 0) {
        memset(&original, 0, sizeof(TagFSMetadata));
    }

    uint32_t txn_id;
    if (journal_begin(&txn_id) != 0) {
        debug_printf("[JournalTest] FAIL: journal_begin failed\n");
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(TagFSMetadata));
    meta.magic = TAGFS_METADATA_MAGIC;
    meta.file_id = 3;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 99999;
    strncpy(meta.filename, "aborted.txt", TAGFS_MAX_FILENAME);

    if (journal_log_metadata(txn_id, 3, &meta) != 0) {
        debug_printf("[JournalTest] FAIL: journal_log_metadata failed\n");
        journal_abort(txn_id);
        return -1;
    }

    journal_abort(txn_id);

    if (journal_replay() != 0) {
        debug_printf("[JournalTest] FAIL: journal_replay after abort failed\n");
        return -1;
    }

    TagFSMetadata read_meta;
    if (tagfs_read_metadata(3, &read_meta) != 0) {
        memset(&read_meta, 0, sizeof(TagFSMetadata));
    }

    if (read_meta.size == 99999) {
        debug_printf("[JournalTest] FAIL: aborted transaction was applied\n");
        return -1;
    }

    debug_printf("[JournalTest] PASS: journal abort\n");
    return 0;
}

static int test_journal_validate_and_replay(void) {
    debug_printf("[JournalTest] Testing journal_validate_and_replay...\n");

    TagFSMetadata saved_meta_5, saved_meta_6;
    int had_saved_5 = (tagfs_read_metadata(5, &saved_meta_5) == 0);
    int had_saved_6 = (tagfs_read_metadata(6, &saved_meta_6) == 0);

    debug_printf("[JournalTest] Phase 1: Create committed transaction\n");
    uint32_t txn_id;
    if (journal_begin(&txn_id) != 0) {
        debug_printf("[JournalTest] FAIL: journal_begin failed\n");
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(TagFSMetadata));
    meta.magic = TAGFS_METADATA_MAGIC;
    meta.file_id = 5;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 4096;
    strncpy(meta.filename, "validated.txt", TAGFS_MAX_FILENAME);

    if (journal_log_metadata(txn_id, 5, &meta) != 0) {
        debug_printf("[JournalTest] FAIL: journal_log_metadata failed\n");
        journal_abort(txn_id);
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    if (journal_commit(txn_id) != 0) {
        debug_printf("[JournalTest] FAIL: journal_commit failed\n");
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    debug_printf("[JournalTest] Phase 2: Create uncommitted transaction (simulates crash)\n");
    uint32_t uncommitted_txn;
    if (journal_begin(&uncommitted_txn) != 0) {
        debug_printf("[JournalTest] FAIL: journal_begin for uncommitted failed\n");
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    TagFSMetadata uncommitted_meta;
    memset(&uncommitted_meta, 0, sizeof(TagFSMetadata));
    uncommitted_meta.magic = TAGFS_METADATA_MAGIC;
    uncommitted_meta.file_id = 6;
    uncommitted_meta.flags = TAGFS_FILE_ACTIVE;
    uncommitted_meta.size = 8192;
    strncpy(uncommitted_meta.filename, "crash.txt", TAGFS_MAX_FILENAME);

    if (journal_log_metadata(uncommitted_txn, 6, &uncommitted_meta) != 0) {
        debug_printf("[JournalTest] FAIL: journal_log_metadata for uncommitted failed\n");
        journal_abort(uncommitted_txn);
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    debug_printf("[JournalTest] Phase 3: Run journal_validate_and_replay\n");
    if (journal_validate_and_replay() != 0) {
        debug_printf("[JournalTest] FAIL: journal_validate_and_replay failed\n");
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    debug_printf("[JournalTest] Phase 4: Verify committed transaction was replayed\n");
    TagFSMetadata read_committed;
    if (tagfs_read_metadata(5, &read_committed) != 0) {
        debug_printf("[JournalTest] FAIL: read_metadata for committed failed\n");
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    if (read_committed.size != 4096 || strcmp(read_committed.filename, "validated.txt") != 0) {
        debug_printf("[JournalTest] FAIL: committed transaction not applied correctly\n");
        debug_printf("[JournalTest]   Expected size=4096, got %llu\n", read_committed.size);
        debug_printf("[JournalTest]   Expected filename='validated.txt', got '%s'\n", read_committed.filename);
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    debug_printf("[JournalTest] Phase 5: Verify uncommitted transaction was skipped\n");
    TagFSMetadata read_uncommitted;
    if (tagfs_read_metadata(6, &read_uncommitted) != 0) {
        memset(&read_uncommitted, 0, sizeof(TagFSMetadata));
    }

    if (read_uncommitted.size == 8192) {
        debug_printf("[JournalTest] FAIL: uncommitted transaction was incorrectly applied\n");
        if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
        if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
        return -1;
    }

    debug_printf("[JournalTest] PASS: journal_validate_and_replay\n");
    if (had_saved_5) tagfs_write_metadata_journaled(5, &saved_meta_5);
    if (had_saved_6) tagfs_write_metadata_journaled(6, &saved_meta_6);
    return 0;
}

static int test_journal_corrupted_magic(void) {
    debug_printf("[JournalTest] Testing corrupted magic number...\n");

    uint8_t* buffer = kmalloc(512);
    if (!buffer) {
        debug_printf("[JournalTest] FAIL: Memory allocation failed\n");
        return -1;
    }

    debug_printf("[JournalTest] Phase 1: Corrupt primary superblock magic\n");
    memset(buffer, 0xFF, 512);

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to write corrupted primary superblock\n");
        kfree(buffer);
        return -1;
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[JournalTest] WARNING: Cache flush failed\n");
    }

    debug_printf("[JournalTest] Phase 2: Write valid backup and reload journal\n");

    JournalSuperblock sb_backup;
    memset(&sb_backup, 0, sizeof(sb_backup));
    sb_backup.magic = JOURNAL_MAGIC;
    sb_backup.version = JOURNAL_VERSION;
    sb_backup.start_sector = JOURNAL_ENTRIES_START;
    sb_backup.entry_count = JOURNAL_ENTRY_COUNT;
    sb_backup.head = 0;
    sb_backup.tail = 0;
    sb_backup.commit_seq = 1;

    memset(buffer, 0, 512);
    memcpy(buffer, &sb_backup, sizeof(sb_backup));

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_BACKUP, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to write valid backup superblock\n");
        kfree(buffer);
        return -1;
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[JournalTest] WARNING: Cache flush failed\n");
    }

    if (journal_reload() != 0) {
        debug_printf("[JournalTest] FAIL: Journal reload failed (should recover from backup)\n");
        kfree(buffer);
        return -1;
    }

    debug_printf("[JournalTest] Phase 3: Verify recovery from backup\n");

    if (ata_read_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to read restored primary\n");
        kfree(buffer);
        return -1;
    }

    JournalSuperblock restored;
    memcpy(&restored, buffer, sizeof(restored));

    if (restored.magic != JOURNAL_MAGIC) {
        debug_printf("[JournalTest] FAIL: Primary not restored (magic=0x%08x)\n", restored.magic);
        kfree(buffer);
        return -1;
    }

    kfree(buffer);
    debug_printf("[JournalTest] PASS: journal_corrupted_magic\n");
    return 0;
}

static int test_journal_superblock_corruption(void) {
    debug_printf("[JournalTest] Testing superblock head/tail corruption...\n");

    uint8_t* buffer = kmalloc(512);
    if (!buffer) {
        debug_printf("[JournalTest] FAIL: Memory allocation failed\n");
        return -1;
    }

    debug_printf("[JournalTest] Phase 1: Create superblock with invalid head/tail\n");

    JournalSuperblock corrupt_sb;
    memset(&corrupt_sb, 0, sizeof(corrupt_sb));
    corrupt_sb.magic = JOURNAL_MAGIC;
    corrupt_sb.version = JOURNAL_VERSION;
    corrupt_sb.start_sector = JOURNAL_ENTRIES_START;
    corrupt_sb.entry_count = JOURNAL_ENTRY_COUNT;
    corrupt_sb.head = 9999;
    corrupt_sb.tail = 8888;
    corrupt_sb.commit_seq = 1;

    memset(buffer, 0, 512);
    memcpy(buffer, &corrupt_sb, sizeof(corrupt_sb));

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to write corrupted superblock\n");
        kfree(buffer);
        return -1;
    }

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_BACKUP, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to write corrupted backup\n");
        kfree(buffer);
        return -1;
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[JournalTest] WARNING: Cache flush failed\n");
    }

    debug_printf("[JournalTest] Phase 2: Reload journal with corrupted superblock\n");

    if (journal_reload() != 0) {
        debug_printf("[JournalTest] FAIL: Journal reload failed\n");
        kfree(buffer);
        return -1;
    }

    debug_printf("[JournalTest] Phase 3: Run validate_and_replay (should detect corruption and reset)\n");

    if (journal_validate_and_replay() != 0) {
        debug_printf("[JournalTest] FAIL: validate_and_replay failed\n");
        kfree(buffer);
        return -1;
    }

    debug_printf("[JournalTest] Phase 4: Verify journal was reset to empty state\n");

    if (ata_read_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        debug_printf("[JournalTest] FAIL: Failed to read superblock\n");
        kfree(buffer);
        return -1;
    }

    JournalSuperblock fixed_sb;
    memcpy(&fixed_sb, buffer, sizeof(fixed_sb));

    if (fixed_sb.head != 0 || fixed_sb.tail != 0) {
        debug_printf("[JournalTest] FAIL: Journal not reset (head=%u, tail=%u)\n",
                     fixed_sb.head, fixed_sb.tail);
        kfree(buffer);
        return -1;
    }

    if (fixed_sb.magic != JOURNAL_MAGIC) {
        debug_printf("[JournalTest] FAIL: Magic corrupted after reset\n");
        kfree(buffer);
        return -1;
    }

    kfree(buffer);
    debug_printf("[JournalTest] PASS: journal_superblock_corruption\n");
    return 0;
}

int run_journal_tests(void) {
    debug_printf("[JournalTest] Starting journal tests...\n");

    int failed = 0;

    if (test_journal_init() != 0) {
        failed++;
    }

    if (test_journal_metadata_write() != 0) {
        failed++;
    }

    if (test_journal_superblock_write() != 0) {
        failed++;
    }

    if (test_journal_replay() != 0) {
        failed++;
    }

    if (test_journal_abort() != 0) {
        failed++;
    }

    if (test_journal_validate_and_replay() != 0) {
        failed++;
    }

    if (test_journal_corrupted_magic() != 0) {
        failed++;
    }

    if (test_journal_superblock_corruption() != 0) {
        failed++;
    }

    if (failed == 0) {
        debug_printf("[JournalTest] All tests passed\n");
        return 0;
    } else {
        debug_printf("[JournalTest] %d tests failed\n", failed);
        return -1;
    }
}
