#include "tagfs.h"
#include "klib.h"

static void test_defrag_empty_file(void) {
    debug_printf("[DEFRAG_TEST] Test 1: Defrag empty file\n");

    const char* tags[] = { "test" };
    uint32_t file_id;

    if (tagfs_create_file("empty.txt", tags, 1, &file_id) != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not create file\n");
        return;
    }

    int result = tagfs_defrag_file(file_id, 0);
    if (result != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Defrag of empty file should succeed (no-op)\n");
    } else {
        debug_printf("[DEFRAG_TEST] PASS: Empty file defrag is no-op\n");
    }

    tagfs_delete_file(file_id);
}

static void test_defrag_with_data(void) {
    debug_printf("[DEFRAG_TEST] Test 2: Defrag file with data\n");

    const char* tags[] = { "test" };
    uint32_t file_id;

    if (tagfs_create_file("data.txt", tags, 1, &file_id) != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not create file\n");
        return;
    }

    char write_buffer[8192];
    for (int i = 0; i < 8192; i++) {
        write_buffer[i] = (char)(i % 256);
    }

    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not open file\n");
        tagfs_delete_file(file_id);
        return;
    }

    int written = tagfs_write(handle, write_buffer, 8192);
    tagfs_close(handle);

    if (written != 8192) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not write test data\n");
        tagfs_delete_file(file_id);
        return;
    }

    TagFSMetadata* meta_before = tagfs_get_metadata(file_id);
    uint32_t start_block_before = meta_before->start_block;
    uint32_t block_count_before = meta_before->block_count;

    if (tagfs_defrag_file(file_id, 0) != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Defragmentation failed\n");
        tagfs_delete_file(file_id);
        return;
    }

    char read_buffer[8192];
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not open file after defrag\n");
        tagfs_delete_file(file_id);
        return;
    }

    int read_bytes = tagfs_read(handle, read_buffer, 8192);
    tagfs_close(handle);

    if (read_bytes != 8192) {
        debug_printf("[DEFRAG_TEST] FAIL: Read size mismatch\n");
        tagfs_delete_file(file_id);
        return;
    }

    bool data_match = true;
    for (int i = 0; i < 8192; i++) {
        if (read_buffer[i] != write_buffer[i]) {
            data_match = false;
            break;
        }
    }

    if (!data_match) {
        debug_printf("[DEFRAG_TEST] FAIL: Data integrity lost after defrag\n");
        tagfs_delete_file(file_id);
        return;
    }

    TagFSMetadata* meta_after = tagfs_get_metadata(file_id);

    if (meta_after->block_count != block_count_before) {
        debug_printf("[DEFRAG_TEST] FAIL: Block count changed\n");
        tagfs_delete_file(file_id);
        return;
    }

    debug_printf("[DEFRAG_TEST] PASS: Data integrity preserved (moved from block %u to %u)\n",
                start_block_before, meta_after->start_block);

    tagfs_delete_file(file_id);
}

static void test_fragmentation_score(void) {
    debug_printf("[DEFRAG_TEST] Test 3: Fragmentation score calculation\n");

    uint32_t score_initial = tagfs_get_fragmentation_score();
    debug_printf("[DEFRAG_TEST] Initial fragmentation score: %u%%\n", score_initial);

    const char* tags[] = { "test" };
    uint32_t file_ids[10];

    for (int i = 0; i < 10; i++) {
        char filename[32];
        ksnprintf(filename, 32, "frag_test_%d.txt", i);

        if (tagfs_create_file(filename, tags, 1, &file_ids[i]) != 0) {
            debug_printf("[DEFRAG_TEST] FAIL: Could not create file %d\n", i);
            return;
        }

        char buffer[4096];
        for (int j = 0; j < 4096; j++) {
            buffer[j] = (char)i;
        }

        TagFSFileHandle* handle = tagfs_open(file_ids[i], TAGFS_HANDLE_WRITE);
        if (handle) {
            tagfs_write(handle, buffer, 4096);
            tagfs_close(handle);
        }
    }

    for (int i = 1; i < 10; i += 2) {
        tagfs_delete_file(file_ids[i]);
    }

    uint32_t score_fragmented = tagfs_get_fragmentation_score();
    debug_printf("[DEFRAG_TEST] After creating gaps: %u%%\n", score_fragmented);

    for (int i = 0; i < 10; i += 2) {
        tagfs_defrag_file(file_ids[i], 0);
    }

    uint32_t score_defragmented = tagfs_get_fragmentation_score();
    debug_printf("[DEFRAG_TEST] After defrag: %u%%\n", score_defragmented);

    if (score_defragmented <= score_fragmented) {
        debug_printf("[DEFRAG_TEST] PASS: Fragmentation reduced or same\n");
    } else {
        debug_printf("[DEFRAG_TEST] FAIL: Fragmentation increased\n");
    }

    for (int i = 0; i < 10; i += 2) {
        tagfs_delete_file(file_ids[i]);
    }
}

static void test_defrag_contiguous_file(void) {
    debug_printf("[DEFRAG_TEST] Test 4: Defrag already contiguous file\n");

    const char* tags[] = { "test" };
    uint32_t file_id;

    if (tagfs_create_file("contiguous.txt", tags, 1, &file_id) != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Could not create file\n");
        return;
    }

    char buffer[4096];
    for (int i = 0; i < 4096; i++) {
        buffer[i] = (char)(i % 256);
    }

    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (handle) {
        tagfs_write(handle, buffer, 4096);
        tagfs_close(handle);
    }

    TagFSMetadata* meta_before = tagfs_get_metadata(file_id);
    uint32_t start_before = meta_before->start_block;

    if (tagfs_defrag_file(file_id, 0) != 0) {
        debug_printf("[DEFRAG_TEST] FAIL: Defrag failed\n");
        tagfs_delete_file(file_id);
        return;
    }

    TagFSMetadata* meta_after = tagfs_get_metadata(file_id);

    debug_printf("[DEFRAG_TEST] PASS: Contiguous file defragged (block %u -> %u)\n",
                start_before, meta_after->start_block);

    tagfs_delete_file(file_id);
}

void tagfs_run_defrag_tests(void) {
    debug_printf("\n========================================\n");
    debug_printf("TAGFS DEFRAGMENTATION TEST SUITE\n");
    debug_printf("========================================\n\n");

    test_defrag_empty_file();
    test_defrag_with_data();
    test_fragmentation_score();
    test_defrag_contiguous_file();

    debug_printf("\n========================================\n");
    debug_printf("DEFRAG TESTS COMPLETE\n");
    debug_printf("========================================\n\n");
}
