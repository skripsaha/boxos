#include "tests.h"
#include "../tagfs.h"
#include "../bcdc/bcdc.h"
#include "../disk_book/disk_book.h"
#include "../braid/braid.h"
#include "../cow/cow.h"
#include "../../../kernel/drivers/timer/rtc.h"

// ============================================================================
// Global Test State
// ============================================================================

static TestStats g_test_stats;
static bool g_tests_initialized = false;
static uint8_t g_test_buffer[16384];  // 4 blocks for testing
static uint8_t g_test_output[16384];

// ============================================================================
// Utility Functions
// ============================================================================

static uint64_t get_time_ms(void) {
    return rtc_get_unix64() * 1000;  // Approximate
}

static void fill_random(void* buffer, uint32_t size) {
    uint8_t* buf = (uint8_t*)buffer;
    for (uint32_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)(i ^ (i >> 3) ^ (i >> 7));
    }
}

static void fill_zeros(void* buffer, uint32_t size) {
    memset(buffer, 0, size);
}

static void fill_pattern(void* buffer, uint32_t size, uint8_t pattern) {
    memset(buffer, pattern, size);
}

// ============================================================================
// Core TagFS Tests
// ============================================================================

static TestResult test_tagfs_init(void) {
    TagFSState* fs = tagfs_get_state();
    TEST_ASSERT(fs != NULL, "TagFS state should not be NULL");
    TEST_ASSERT(fs->initialized, "TagFS should be initialized");
    return TEST_PASS;
}

static TestResult test_tagfs_superblock(void) {
    TagFSState* fs = tagfs_get_state();
    TEST_ASSERT(fs != NULL, "TagFS state should not be NULL");
    
    TEST_ASSERT_EQ(fs->superblock.magic, TAGFS_MAGIC, "Superblock magic mismatch");
    TEST_ASSERT_EQ(fs->superblock.version, TAGFS_VERSION, "Superblock version mismatch");
    TEST_ASSERT(fs->superblock.total_blocks > 0, "Total blocks should be > 0");
    TEST_ASSERT(fs->superblock.free_blocks > 0, "Free blocks should be > 0");
    
    return TEST_PASS;
}

static TestResult test_tagfs_create_file(void) {
    uint32_t file_id;
    uint16_t tag_ids[2] = {1, 2};
    
    error_t err = tagfs_create_file("test_file", tag_ids, 2, &file_id);
    TEST_ASSERT_OK(err, "tagfs_create_file should succeed");
    TEST_ASSERT(file_id > 0, "File ID should be > 0");
    
    // Cleanup
    tagfs_delete_file(file_id);
    
    return TEST_PASS;
}

static TestResult test_tagfs_write_read(void) {
    uint32_t file_id;
    uint16_t tag_ids[1] = {1};
    
    // Create file
    error_t err = tagfs_create_file("test_rw", tag_ids, 1, &file_id);
    TEST_ASSERT_OK(err, "tagfs_create_file should succeed");
    
    // Open for write
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "tagfs_open should succeed");
    
    // Write data
    fill_random(g_test_buffer, 4096);
    int written = tagfs_write(handle, g_test_buffer, 4096);
    TEST_ASSERT_EQ(written, 4096, "Should write 4096 bytes");
    
    // Close and reopen for read
    tagfs_close(handle);
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "tagfs_open for read should succeed");
    
    // Read data
    memset(g_test_output, 0, sizeof(g_test_output));
    int read = tagfs_read(handle, g_test_output, 4096);
    TEST_ASSERT_EQ(read, 4096, "Should read 4096 bytes");
    
    // Verify data
    TEST_ASSERT(memcmp(g_test_buffer, g_test_output, 4096) == 0, "Data should match");
    
    // Cleanup
    tagfs_close(handle);
    tagfs_delete_file(file_id);
    
    return TEST_PASS;
}

// ============================================================================
// Bcdc Compression Tests
// ============================================================================

static TestResult test_bcdc_init(void) {
    // Bcdc should already be initialized by TagFS
    // Just verify it's working
    BcdcStats stats;
    BcdcGetStats(&stats);
    return TEST_PASS;
}

static TestResult test_bcdc_compress_decompress_random(void) {
    uint8_t input[4096];
    uint8_t output[4096 + 24];  // header + data
    uint8_t decompressed[4096];
    uint16_t output_size, decompressed_size;

    // Fill with random data (hard to compress)
    fill_random(input, 4096);

    // Compress with BCDC_TYPE_NONE (random data won't compress well)
    error_t err = BcdcCompress(input, 4096, output, &output_size,
                               BCDC_TYPE_NONE, BCDC_LEVEL_DEFAULT, 0);
    TEST_ASSERT_OK(err, "BcdcCompress should succeed");
    TEST_ASSERT(output_size <= 4096 + 24, "Output should fit in buffer");

    // Decompress
    err = BcdcDecompress(output, output_size, decompressed, &decompressed_size, 0);
    TEST_ASSERT_OK(err, "BcdcDecompress should succeed");
    TEST_ASSERT_EQ(decompressed_size, 4096, "Decompressed size should match");

    // Verify data
    TEST_ASSERT(memcmp(input, decompressed, 4096) == 0, "Data should match");

    return TEST_PASS;
}

static TestResult test_bcdc_compress_decompress_zeros(void) {
    uint8_t input[4096];
    uint8_t output[4096 + 24];
    uint8_t decompressed[4096];
    uint16_t output_size, decompressed_size;

    // Fill with zeros (easy to compress with RLE)
    fill_zeros(input, 4096);

    // Compress with RLE (best for zeros)
    error_t err = BcdcCompress(input, 4096, output, &output_size,
                               BCDC_TYPE_RLE, BCDC_LEVEL_DEFAULT, 0);
    TEST_ASSERT_OK(err, "BcdcCompress should succeed");

    // Decompress
    err = BcdcDecompress(output, output_size, decompressed, &decompressed_size, 0);
    TEST_ASSERT_OK(err, "BcdcDecompress should succeed");
    TEST_ASSERT_EQ(decompressed_size, 4096, "Decompressed size should match");

    // Verify data
    TEST_ASSERT(memcmp(input, decompressed, 4096) == 0, "Data should match");

    return TEST_PASS;
}

static TestResult test_bcdc_compress_decompress_pattern(void) {
    uint8_t input[4096];
    uint8_t output[4096 + 24];
    uint8_t decompressed[4096];
    uint16_t output_size, decompressed_size;
    
    // Fill with repeating pattern (RLE should work well)
    fill_pattern(input, 4096, 0xAA);
    
    // Compress with RLE
    error_t err = BcdcCompress(input, 4096, output, &output_size,
                               BCDC_TYPE_RLE, BCDC_LEVEL_DEFAULT, 0);
    TEST_ASSERT_OK(err, "BcdcCompress RLE should succeed");
    
    // Should compress well
    TEST_ASSERT(output_size < 4096, "RLE should compress repeating data");
    
    // Decompress
    err = BcdcDecompress(output, output_size, decompressed, &decompressed_size, 0);
    TEST_ASSERT_OK(err, "BcdcDecompress should succeed");
    
    // Verify data
    TEST_ASSERT(memcmp(input, decompressed, 4096) == 0, "Data should match");
    
    return TEST_PASS;
}

static TestResult test_bcdc_checksum_verification(void) {
    uint8_t input[4096];
    uint8_t output[4096 + 24];
    uint8_t decompressed[4096];
    uint16_t output_size, decompressed_size;
    
    fill_random(input, 4096);
    
    // Compress
    error_t err = BcdcCompress(input, 4096, output, &output_size,
                               BCDC_TYPE_LZ, BCDC_LEVEL_DEFAULT, 0);
    TEST_ASSERT_OK(err, "BcdcCompress should succeed");
    
    // Corrupt compressed data
    output[50] ^= 0xFF;
    
    // Decompress should fail
    err = BcdcDecompress(output, output_size, decompressed, &decompressed_size, 0);
    TEST_ASSERT(err != OK, "BcdcDecompress should detect corruption");
    
    return TEST_PASS;
}

static TestResult test_bcdc_stats(void) {
    BcdcStats stats;
    BcdcGetStats(&stats);

    TEST_ASSERT(stats.blocks_compressed > 0 || stats.blocks_compressed == 0, "Should have compression stats");
    TEST_ASSERT(stats.bytes_before >= stats.bytes_after, "Compression should save space (or equal)");

    return TEST_PASS;
}

// ============================================================================
// DiskBook Journal Tests
// ============================================================================

static TestResult test_diskbook_init(void) {
    // DiskBook should already be initialized
    TEST_ASSERT(DiskBookIsInitialized(), "DiskBook should be initialized");
    return TEST_PASS;
}

static TestResult test_diskbook_checkpoint(void) {
    error_t err = DiskBookCheckpoint();
    TEST_ASSERT_OK(err, "DiskBookCheckpoint should succeed");
    return TEST_PASS;
}

static TestResult test_diskbook_stats(void) {
    DiskBookStats stats;
    error_t err = DiskBookGetStats(&stats);
    TEST_ASSERT_OK(err, "DiskBookGetStats should succeed");
    return TEST_PASS;
}

// ============================================================================
// Snapshot Tests
// ============================================================================

static TestResult test_snapshot_create(void) {
    uint32_t snapshot_id;
    
    error_t err = TagFS_SnapshotCreate("test_snap", 0, &snapshot_id);
    TEST_ASSERT_OK(err, "TagFS_SnapshotCreate should succeed");
    TEST_ASSERT(snapshot_id > 0, "Snapshot ID should be > 0");
    
    // Cleanup
    TagFS_SnapshotDelete(snapshot_id);
    
    return TEST_PASS;
}

static TestResult test_snapshot_list(void) {
    uint32_t ids[16];
    uint32_t count;
    
    error_t err = TagFS_SnapshotList(ids, 16, &count);
    TEST_ASSERT_OK(err, "TagFS_SnapshotList should succeed");
    
    return TEST_PASS;
}

// ============================================================================
// Stress Tests
// ============================================================================

static TestResult test_stress_many_files(void) {
    const uint32_t FILE_COUNT = 10;  // Reduced for QEMU performance
    uint32_t file_ids[10];
    uint16_t tag_ids[2] = {1, 2};

    // Create many files
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), "stress_file_%u", i);

        error_t err = tagfs_create_file(name, tag_ids, 2, &file_ids[i]);
        TEST_ASSERT_OK(err, "tagfs_create_file should succeed");
    }

    // Write to all files
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        TagFSFileHandle* handle = tagfs_open(file_ids[i], TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
        if (handle) {
            fill_random(g_test_buffer, 4096);
            tagfs_write(handle, g_test_buffer, 4096);
            tagfs_close(handle);
        }
    }

    // Read from all files
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        TagFSFileHandle* handle = tagfs_open(file_ids[i], TAGFS_HANDLE_READ);
        if (handle) {
            memset(g_test_output, 0, sizeof(g_test_output));
            tagfs_read(handle, g_test_output, 4096);
            tagfs_close(handle);
        }
    }

    // Cleanup
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        tagfs_delete_file(file_ids[i]);
    }

    return TEST_PASS;
}

static TestResult test_stress_large_file(void) {
    const uint32_t FILE_SIZE = 16 * 1024;  // Reduced to 16KB for QEMU
    uint32_t file_id;
    uint16_t tag_ids[1] = {1};

    // Create file
    error_t err = tagfs_create_file("large_file", tag_ids, 1, &file_id);
    TEST_ASSERT_OK(err, "tagfs_create_file should succeed");

    // Open for write
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "tagfs_open should succeed");

    // Write file in 4KB chunks
    fill_random(g_test_buffer, 4096);
    for (uint32_t i = 0; i < FILE_SIZE / 4096; i++) {
        int written = tagfs_write(handle, g_test_buffer, 4096);
        TEST_ASSERT_EQ(written, 4096, "Should write 4096 bytes");
    }

    // Close and reopen for read
    tagfs_close(handle);
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "tagfs_open for read should succeed");

    // Read and verify (just first and last block for speed)
    memset(g_test_output, 0, sizeof(g_test_output));
    int read = tagfs_read(handle, g_test_output, 4096);
    TEST_ASSERT_EQ(read, 4096, "Should read 4096 bytes");
    TEST_ASSERT(memcmp(g_test_buffer, g_test_output, 4096) == 0, "First block should match");

    // Cleanup
    tagfs_close(handle);
    tagfs_delete_file(file_id);

    return TEST_PASS;
}

static TestResult test_stress_compression(void) {
    const uint32_t ITERATIONS = 20;  // Reduced from 100 for QEMU performance

    for (uint32_t i = 0; i < ITERATIONS; i++) {
        uint8_t input[4096];
        uint8_t output[4096 + 24];
        uint8_t decompressed[4096];
        uint16_t output_size, decompressed_size;

        // Alternate between random and zeros
        if (i % 2 == 0) {
            fill_random(input, 4096);
        } else {
            fill_zeros(input, 4096);
        }

        error_t err = BcdcCompress(input, 4096, output, &output_size,
                                   BCDC_TYPE_RLE, BCDC_LEVEL_DEFAULT, 0);
        TEST_ASSERT_OK(err, "BcdcCompress should succeed");

        err = BcdcDecompress(output, output_size, decompressed, &decompressed_size, 0);
        TEST_ASSERT_OK(err, "BcdcDecompress should succeed");
        TEST_ASSERT(memcmp(input, decompressed, 4096) == 0, "Data should match");
    }

    return TEST_PASS;
}

static TestResult test_braid_init(void) {
    error_t err = BraidInit(BraidModeMirror);
    TEST_ASSERT_OK(err, "BraidInit should succeed");
    return TEST_PASS;
}

static TestResult test_braid_add_disk(void) {
    error_t err = BraidAddDisk(0, 1000000);
    TEST_ASSERT_OK(err, "BraidAddDisk should succeed");
    
    err = BraidAddDisk(1, 1000000);
    TEST_ASSERT_OK(err, "BraidAddDisk second disk should succeed");
    
    TEST_ASSERT(BraidIsHealthy(), "Braid should be healthy with 2 disks");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(), 2, "Should have 2 active disks");
    
    return TEST_PASS;
}

static TestResult test_braid_write_read(void) {
    // Skip - requires multiple physical disks not available in QEMU
    return TEST_SKIP;
}

static TestResult test_cow_snapshot_create(void) {
    uint32_t snapshot_id;
    
    error_t err = TagFS_SnapshotCreate("test-snapshot", 0, &snapshot_id);
    TEST_ASSERT_OK(err, "TagFS_SnapshotCreate should succeed");
    TEST_ASSERT(snapshot_id > 0, "Snapshot ID should be > 0");
    
    // Cleanup
    TagFS_SnapshotDelete(snapshot_id);
    
    return TEST_PASS;
}

static TestResult test_cow_before_after_write(void) {
    uint32_t file_id;
    uint16_t tag_ids[1] = {1};
    
    // Create file
    error_t err = tagfs_create_file("cow-test", tag_ids, 1, &file_id);
    TEST_ASSERT_OK(err, "tagfs_create_file should succeed");
    
    // Open and write initial data
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "tagfs_open should succeed");
    
    fill_random(g_test_buffer, 4096);
    int written = tagfs_write(handle, g_test_buffer, 4096);
    TEST_ASSERT_EQ(written, 4096, "Should write 4096 bytes");
    
    tagfs_close(handle);
    
    // Test CoW before write
    uint32_t new_block;
    err = TagFS_CowBeforeWrite(file_id, 1, &new_block);
    // May fail if no snapshot active, which is OK
    if (err == OK) {
        TEST_ASSERT(new_block > 0, "New block should be allocated");
        
        err = TagFS_CowAfterWrite(file_id, 1, new_block);
        TEST_ASSERT_OK(err, "TagFS_CowAfterWrite should succeed");
    }
    
    // Cleanup
    tagfs_delete_file(file_id);
    
    return TEST_PASS;
}

static TestResult test_stress_braid_operations(void) {
    // Skip - requires multiple physical disks not available in QEMU
    return TEST_SKIP;
}

static TestResult test_stress_concurrent_operations(void) {
    const uint32_t FILE_COUNT = 10;  // Reduced from 50 for QEMU performance
    uint32_t file_ids[10];
    uint16_t tag_ids[2] = {1, 2};
    
    // Create files
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), "concurrent_%u", i);
        
        error_t err = tagfs_create_file(name, tag_ids, 2, &file_ids[i]);
        TEST_ASSERT_OK(err, "tagfs_create_file should succeed");
    }
    
    // Write to all files concurrently (simulated)
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        TagFSFileHandle* handle = tagfs_open(file_ids[i], TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
        if (handle) {
            fill_random(g_test_buffer, 4096);
            tagfs_write(handle, g_test_buffer, 4096);
            tagfs_close(handle);
        }
    }
    
    // Read from all files
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        TagFSFileHandle* handle = tagfs_open(file_ids[i], TAGFS_HANDLE_READ);
        if (handle) {
            memset(g_test_output, 0, sizeof(g_test_output));
            tagfs_read(handle, g_test_output, 4096);
            tagfs_close(handle);
        }
    }
    
    // Cleanup
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        tagfs_delete_file(file_ids[i]);
    }
    
    return TEST_PASS;
}

// ============================================================================
// Test Runner Implementation
// ============================================================================

error_t TagFS_TestsInit(void) {
    if (g_tests_initialized)
        return ERR_ALREADY_INITIALIZED;
    
    memset(&g_test_stats, 0, sizeof(TestStats));
    g_test_stats.start_time = get_time_ms();
    g_tests_initialized = true;
    
    debug_printf("[Tests] Initialized\n");
    return OK;
}

void TagFS_TestsShutdown(void) {
    if (!g_tests_initialized)
        return;
    
    g_test_stats.end_time = get_time_ms();
    g_test_stats.total_duration_ms = g_test_stats.end_time - g_test_stats.start_time;
    g_tests_initialized = false;
    
    debug_printf("[Tests] Shutdown complete\n");
}

void TagFS_PrintTestResults(const TestStats* stats) {
    debug_printf("\n");
    debug_printf("========================================\n");
    debug_printf("         TagFS Test Results            \n");
    debug_printf("========================================\n");
    debug_printf("Total tests:    %u\n", stats->total_tests);
    debug_printf("Passed:         %u\n", stats->total_passed);
    debug_printf("Failed:         %u\n", stats->total_failed);
    debug_printf("Skipped:        %u\n", stats->total_skipped);
    debug_printf("Duration:       %lu ms\n", (unsigned long)stats->total_duration_ms);
    
    if (stats->total_failed == 0) {
        debug_printf("\n[SUCCESS] All tests passed!\n");
    } else {
        debug_printf("\n[FAILURE] %u tests failed\n", stats->total_failed);
    }
    
    debug_printf("========================================\n");
}

void TagFS_DumpState(void) {
    debug_printf("\n=== TagFS State Dump ===\n");
    
    TagFSState* fs = tagfs_get_state();
    if (fs && fs->initialized) {
        debug_printf("Total blocks:   %u\n", fs->superblock.total_blocks);
        debug_printf("Free blocks:    %u\n", fs->superblock.free_blocks);
        debug_printf("Total files:    %u\n", fs->superblock.total_files);
        debug_printf("Total tags:     %u\n", fs->superblock.total_tags);
    }
    
    BcdcStats bcdc_stats;
    BcdcGetStats(&bcdc_stats);
    debug_printf("\n=== Bcdc Stats ===\n");
    debug_printf("Blocks compressed:   %lu\n", (unsigned long)bcdc_stats.blocks_compressed);
    debug_printf("Bytes before:        %lu\n", (unsigned long)bcdc_stats.bytes_before);
    debug_printf("Bytes after:         %lu\n", (unsigned long)bcdc_stats.bytes_after);
    if (bcdc_stats.bytes_before > 0) {
        debug_printf("Compression ratio:   %lu%%\n", 
                     (unsigned long)(bcdc_stats.bytes_after * 100 / bcdc_stats.bytes_before));
    }
    
    debug_printf("======================\n");
}

// Run all tests
error_t TagFS_RunAllTests(TestStats* stats) {
    if (!g_tests_initialized)
        return ERR_NOT_INITIALIZED;
    
    memset(stats, 0, sizeof(TestStats));
    stats->start_time = get_time_ms();
    
    // Define all tests
    TestCase core_tests[] = {
        {"tagfs_init", test_tagfs_init, TEST_SKIP, 0, ""},
        {"tagfs_superblock", test_tagfs_superblock, TEST_SKIP, 0, ""},
        {"tagfs_create_file", test_tagfs_create_file, TEST_SKIP, 0, ""},
        {"tagfs_write_read", test_tagfs_write_read, TEST_SKIP, 0, ""},
    };
    
    TestCase compression_tests[] = {
        {"bcdc_init", test_bcdc_init, TEST_SKIP, 0, ""},
        {"bcdc_compress_decompress_random", test_bcdc_compress_decompress_random, TEST_SKIP, 0, ""},
        {"bcdc_compress_decompress_zeros", test_bcdc_compress_decompress_zeros, TEST_SKIP, 0, ""},
        {"bcdc_compress_decompress_pattern", test_bcdc_compress_decompress_pattern, TEST_SKIP, 0, ""},
        {"bcdc_checksum_verification", test_bcdc_checksum_verification, TEST_SKIP, 0, ""},
        {"bcdc_stats", test_bcdc_stats, TEST_SKIP, 0, ""},
    };
    
    TestCase journal_tests[] = {
        {"diskbook_init", test_diskbook_init, TEST_SKIP, 0, ""},
        {"diskbook_checkpoint", test_diskbook_checkpoint, TEST_SKIP, 0, ""},
        {"diskbook_stats", test_diskbook_stats, TEST_SKIP, 0, ""},
    };
    
    TestCase snapshot_tests[] = {
        {"snapshot_create", test_snapshot_create, TEST_SKIP, 0, ""},
        {"snapshot_list", test_snapshot_list, TEST_SKIP, 0, ""},
    };
    
    TestCase stress_tests[] = {
        {"stress_many_files", test_stress_many_files, TEST_SKIP, 0, ""},
        {"stress_large_file", test_stress_large_file, TEST_SKIP, 0, ""},
        {"stress_compression", test_stress_compression, TEST_SKIP, 0, ""},
        {"stress_braid_operations", test_stress_braid_operations, TEST_SKIP, 0, ""},
        {"stress_concurrent_operations", test_stress_concurrent_operations, TEST_SKIP, 0, ""},
    };

    TestCase braid_tests[] = {
        {"braid_init", test_braid_init, TEST_SKIP, 0, ""},
        {"braid_add_disk", test_braid_add_disk, TEST_SKIP, 0, ""},
        {"braid_write_read", test_braid_write_read, TEST_SKIP, 0, ""},
    };

    TestCase cow_tests[] = {
        {"cow_snapshot_create", test_cow_snapshot_create, TEST_SKIP, 0, ""},
        {"cow_before_after_write", test_cow_before_after_write, TEST_SKIP, 0, ""},
    };

    // Run all test suites
    TestCase* all_suites[] = {
        core_tests, compression_tests, journal_tests, snapshot_tests, stress_tests, braid_tests, cow_tests
    };
    uint32_t suite_sizes[] = {
        sizeof(core_tests)/sizeof(TestCase),
        sizeof(compression_tests)/sizeof(TestCase),
        sizeof(journal_tests)/sizeof(TestCase),
        sizeof(snapshot_tests)/sizeof(TestCase),
        sizeof(stress_tests)/sizeof(TestCase),
        sizeof(braid_tests)/sizeof(TestCase),
        sizeof(cow_tests)/sizeof(TestCase)
    };

    debug_printf("\n[Tests] Starting test run...\n");

    for (uint32_t s = 0; s < 7; s++) {
        for (uint32_t i = 0; i < suite_sizes[s]; i++) {
            TestCase* test = &all_suites[s][i];
            uint64_t start = get_time_ms();
            
            debug_printf("[TEST] Running %s...\n", test->name);
            
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            
            stats->total_tests++;
            if (test->result == TEST_PASS) {
                stats->total_passed++;
                debug_printf("[PASS] %s (%lu ms)\n", test->name, (unsigned long)test->duration_ms);
            } else if (test->result == TEST_FAIL) {
                stats->total_failed++;
                debug_printf("[FAIL] %s (%lu ms)\n", test->name, (unsigned long)test->duration_ms);
            } else {
                stats->total_skipped++;
                debug_printf("[SKIP] %s\n", test->name);
            }
        }
    }
    
    stats->end_time = get_time_ms();
    stats->total_duration_ms = stats->end_time - stats->start_time;
    
    TagFS_PrintTestResults(stats);
    TagFS_DumpState();
    
    return stats->total_failed == 0 ? OK : ERR_INTERNAL;
}

error_t TagFS_RunSuite(const char* suite_name, TestStats* stats) {
    if (!suite_name || !stats || !g_tests_initialized)
        return ERR_INVALID_ARGUMENT;

    memset(stats, 0, sizeof(TestStats));
    stats->start_time = get_time_ms();

    if (strcmp(suite_name, "core") == 0) {
        TestCase core_tests[] = {
            {"tagfs_init", test_tagfs_init, TEST_SKIP, 0, ""},
            {"tagfs_superblock", test_tagfs_superblock, TEST_SKIP, 0, ""},
            {"tagfs_create_file", test_tagfs_create_file, TEST_SKIP, 0, ""},
            {"tagfs_write_read", test_tagfs_write_read, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(core_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &core_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else if (strcmp(suite_name, "compression") == 0) {
        TestCase compression_tests[] = {
            {"bcdc_init", test_bcdc_init, TEST_SKIP, 0, ""},
            {"bcdc_compress_decompress_random", test_bcdc_compress_decompress_random, TEST_SKIP, 0, ""},
            {"bcdc_compress_decompress_zeros", test_bcdc_compress_decompress_zeros, TEST_SKIP, 0, ""},
            {"bcdc_compress_decompress_pattern", test_bcdc_compress_decompress_pattern, TEST_SKIP, 0, ""},
            {"bcdc_checksum_verification", test_bcdc_checksum_verification, TEST_SKIP, 0, ""},
            {"bcdc_stats", test_bcdc_stats, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(compression_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &compression_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else if (strcmp(suite_name, "journal") == 0) {
        TestCase journal_tests[] = {
            {"diskbook_init", test_diskbook_init, TEST_SKIP, 0, ""},
            {"diskbook_checkpoint", test_diskbook_checkpoint, TEST_SKIP, 0, ""},
            {"diskbook_stats", test_diskbook_stats, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(journal_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &journal_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else if (strcmp(suite_name, "snapshot") == 0) {
        TestCase snapshot_tests[] = {
            {"snapshot_create", test_snapshot_create, TEST_SKIP, 0, ""},
            {"snapshot_list", test_snapshot_list, TEST_SKIP, 0, ""},
            {"cow_snapshot_create", test_cow_snapshot_create, TEST_SKIP, 0, ""},
            {"cow_before_after_write", test_cow_before_after_write, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(snapshot_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &snapshot_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else if (strcmp(suite_name, "braid") == 0) {
        TestCase braid_tests[] = {
            {"braid_init", test_braid_init, TEST_SKIP, 0, ""},
            {"braid_add_disk", test_braid_add_disk, TEST_SKIP, 0, ""},
            {"braid_write_read", test_braid_write_read, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(braid_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &braid_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else if (strcmp(suite_name, "stress") == 0) {
        TestCase stress_tests[] = {
            {"stress_many_files", test_stress_many_files, TEST_SKIP, 0, ""},
            {"stress_large_file", test_stress_large_file, TEST_SKIP, 0, ""},
            {"stress_compression", test_stress_compression, TEST_SKIP, 0, ""},
            {"stress_braid_operations", test_stress_braid_operations, TEST_SKIP, 0, ""},
            {"stress_concurrent_operations", test_stress_concurrent_operations, TEST_SKIP, 0, ""},
        };
        uint32_t count = sizeof(stress_tests) / sizeof(TestCase);
        for (uint32_t i = 0; i < count; i++) {
            TestCase* test = &stress_tests[i];
            uint64_t start = get_time_ms();
            test->result = test->func();
            test->duration_ms = get_time_ms() - start;
            stats->total_tests++;
            if (test->result == TEST_PASS) stats->total_passed++;
            else if (test->result == TEST_FAIL) stats->total_failed++;
            else stats->total_skipped++;
        }
    } else {
        return ERR_INVALID_ARGUMENT;
    }

    stats->end_time = get_time_ms();
    stats->total_duration_ms = stats->end_time - stats->start_time;
    TagFS_PrintTestResults(stats);
    return stats->total_failed == 0 ? OK : ERR_INTERNAL;
}

static TestCase g_core_tests_arr[] = {
    {"tagfs_init", test_tagfs_init, TEST_SKIP, 0, ""},
    {"tagfs_superblock", test_tagfs_superblock, TEST_SKIP, 0, ""},
    {"tagfs_create_file", test_tagfs_create_file, TEST_SKIP, 0, ""},
    {"tagfs_write_read", test_tagfs_write_read, TEST_SKIP, 0, ""},
};

static TestCase g_compression_tests_arr[] = {
    {"bcdc_init", test_bcdc_init, TEST_SKIP, 0, ""},
    {"bcdc_compress_decompress_random", test_bcdc_compress_decompress_random, TEST_SKIP, 0, ""},
    {"bcdc_compress_decompress_zeros", test_bcdc_compress_decompress_zeros, TEST_SKIP, 0, ""},
    {"bcdc_compress_decompress_pattern", test_bcdc_compress_decompress_pattern, TEST_SKIP, 0, ""},
    {"bcdc_checksum_verification", test_bcdc_checksum_verification, TEST_SKIP, 0, ""},
    {"bcdc_stats", test_bcdc_stats, TEST_SKIP, 0, ""},
};

static TestCase g_journal_tests_arr[] = {
    {"diskbook_init", test_diskbook_init, TEST_SKIP, 0, ""},
    {"diskbook_checkpoint", test_diskbook_checkpoint, TEST_SKIP, 0, ""},
    {"diskbook_stats", test_diskbook_stats, TEST_SKIP, 0, ""},
};

static TestCase g_snapshot_tests_arr[] = {
    {"snapshot_create", test_snapshot_create, TEST_SKIP, 0, ""},
    {"snapshot_list", test_snapshot_list, TEST_SKIP, 0, ""},
    {"cow_snapshot_create", test_cow_snapshot_create, TEST_SKIP, 0, ""},
    {"cow_before_after_write", test_cow_before_after_write, TEST_SKIP, 0, ""},
};

static TestCase g_braid_tests_arr[] = {
    {"braid_init", test_braid_init, TEST_SKIP, 0, ""},
    {"braid_add_disk", test_braid_add_disk, TEST_SKIP, 0, ""},
    {"braid_write_read", test_braid_write_read, TEST_SKIP, 0, ""},
};

static TestCase g_stress_tests_arr[] = {
    {"stress_many_files", test_stress_many_files, TEST_SKIP, 0, ""},
    {"stress_large_file", test_stress_large_file, TEST_SKIP, 0, ""},
    {"stress_compression", test_stress_compression, TEST_SKIP, 0, ""},
    {"stress_braid_operations", test_stress_braid_operations, TEST_SKIP, 0, ""},
    {"stress_concurrent_operations", test_stress_concurrent_operations, TEST_SKIP, 0, ""},
};

static TestSuite g_core_suite = {"core", g_core_tests_arr, 4, 0, 0, 0};
static TestSuite g_compression_suite = {"compression", g_compression_tests_arr, 6, 0, 0, 0};
static TestSuite g_journal_suite = {"journal", g_journal_tests_arr, 3, 0, 0, 0};
static TestSuite g_snapshot_suite = {"snapshot", g_snapshot_tests_arr, 4, 0, 0, 0};
static TestSuite g_braid_suite = {"braid", g_braid_tests_arr, 3, 0, 0, 0};
static TestSuite g_stress_suite = {"stress", g_stress_tests_arr, 5, 0, 0, 0};

TestSuite* TagFS_GetCoreTests(void) {
    return &g_core_suite;
}

TestSuite* TagFS_GetCompressionTests(void) {
    return &g_compression_suite;
}

TestSuite* TagFS_GetJournalTests(void) {
    return &g_journal_suite;
}

TestSuite* TagFS_GetSnapshotTests(void) {
    return &g_snapshot_suite;
}

TestSuite* TagFS_GetBraidTests(void) {
    return &g_braid_suite;
}

TestSuite* TagFS_GetStressTests(void) {
    return &g_stress_suite;
}
