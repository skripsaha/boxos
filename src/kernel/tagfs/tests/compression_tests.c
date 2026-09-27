#include "tests.h"
#include "../tagfs.h"
#include "../bcdc/bcdc.h"

// ============================================================================
// Compression (Bcdc) — a block in, a block out, the same block
//
// The three buffers are carved out of the shared ones rather than put on the
// stack: the compressed form of a block can run to 4096 + 24 bytes.
// ============================================================================

#define BCDC_TEST_BYTES 4096

static uint8_t* const s_input        = g_test_buffer;
static uint8_t* const s_squeezed     = g_test_buffer + 2 * BCDC_TEST_BYTES;
static uint8_t* const s_decompressed = g_test_output;

static TestResult test_bcdc_compress_decompress_random(void)
{
    TestResult verdict = TEST_PASS;
    uint16_t   squeezed_size = 0, decompressed_size = 0;

    // Random data won't compress well, so it goes through BCDC_TYPE_NONE
    fill_random(s_input, BCDC_TEST_BYTES);

    TEST_ASSERT_OK(BcdcCompress(s_input, BCDC_TEST_BYTES, s_squeezed, &squeezed_size,
                                BCDC_TYPE_NONE, BCDC_LEVEL_DEFAULT, 0),
                   "BcdcCompress should succeed");
    TEST_ASSERT(squeezed_size <= BCDC_TEST_BYTES + 24, "Output should fit in buffer");

    TEST_ASSERT_OK(BcdcDecompress(s_squeezed, squeezed_size, s_decompressed,
                                  &decompressed_size, 0),
                   "BcdcDecompress should succeed");
    TEST_ASSERT_EQ(decompressed_size, BCDC_TEST_BYTES, "Decompressed size should match");
    TEST_ASSERT_EQ(TestFirstDifference(s_input, s_decompressed, BCDC_TEST_BYTES),
                   BCDC_TEST_BYTES, "Data should match (first differing byte)");
out:
    return verdict;
}

static TestResult test_bcdc_compress_decompress_zeros(void)
{
    TestResult verdict = TEST_PASS;
    uint16_t   squeezed_size = 0, decompressed_size = 0;

    // Zeros are what RLE is best at
    fill_zeros(s_input, BCDC_TEST_BYTES);

    TEST_ASSERT_OK(BcdcCompress(s_input, BCDC_TEST_BYTES, s_squeezed, &squeezed_size,
                                BCDC_TYPE_RLE, BCDC_LEVEL_DEFAULT, 0),
                   "BcdcCompress should succeed");
    TEST_ASSERT_OK(BcdcDecompress(s_squeezed, squeezed_size, s_decompressed,
                                  &decompressed_size, 0),
                   "BcdcDecompress should succeed");
    TEST_ASSERT_EQ(decompressed_size, BCDC_TEST_BYTES, "Decompressed size should match");
    TEST_ASSERT_EQ(TestFirstDifference(s_input, s_decompressed, BCDC_TEST_BYTES),
                   BCDC_TEST_BYTES, "Data should match (first differing byte)");
out:
    return verdict;
}

static TestResult test_bcdc_compress_decompress_pattern(void)
{
    TestResult verdict = TEST_PASS;
    uint16_t   squeezed_size = 0, decompressed_size = 0;

    fill_pattern(s_input, BCDC_TEST_BYTES, 0xAA);

    TEST_ASSERT_OK(BcdcCompress(s_input, BCDC_TEST_BYTES, s_squeezed, &squeezed_size,
                                BCDC_TYPE_RLE, BCDC_LEVEL_DEFAULT, 0),
                   "BcdcCompress RLE should succeed");
    TEST_ASSERT(squeezed_size < BCDC_TEST_BYTES, "RLE should compress repeating data");

    TEST_ASSERT_OK(BcdcDecompress(s_squeezed, squeezed_size, s_decompressed,
                                  &decompressed_size, 0),
                   "BcdcDecompress should succeed");
    TEST_ASSERT_EQ(TestFirstDifference(s_input, s_decompressed, BCDC_TEST_BYTES),
                   BCDC_TEST_BYTES, "Data should match (first differing byte)");
out:
    return verdict;
}

static TestResult test_bcdc_checksum_verification(void)
{
    TestResult verdict = TEST_PASS;
    uint16_t   squeezed_size = 0, decompressed_size = 0;

    fill_random(s_input, BCDC_TEST_BYTES);

    TEST_ASSERT_OK(BcdcCompress(s_input, BCDC_TEST_BYTES, s_squeezed, &squeezed_size,
                                BCDC_TYPE_LZ, BCDC_LEVEL_DEFAULT, 0),
                   "BcdcCompress should succeed");

    s_squeezed[50] ^= 0xFF;     // corrupt the compressed form

    TEST_ASSERT(BcdcDecompress(s_squeezed, squeezed_size, s_decompressed,
                               &decompressed_size, 0) != OK,
                "BcdcDecompress should detect corruption");
out:
    return verdict;
}

static const TestCase g_compression_tests[] = {
    { .name = "bcdc_compress_decompress_random",  .func = test_bcdc_compress_decompress_random,  .reach = TEST_READS_ONLY },
    { .name = "bcdc_compress_decompress_zeros",   .func = test_bcdc_compress_decompress_zeros,   .reach = TEST_READS_ONLY },
    { .name = "bcdc_compress_decompress_pattern", .func = test_bcdc_compress_decompress_pattern, .reach = TEST_READS_ONLY },
    { .name = "bcdc_checksum_verification",       .func = test_bcdc_checksum_verification,       .reach = TEST_READS_ONLY },
};

const TestSuite g_compression_suite = {
    .tests = g_compression_tests, .count = ARRAY_SIZE(g_compression_tests),
};
