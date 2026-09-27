#include "tests.h"
#include "../tagfs.h"
#include "../box_hash/box_hash.h"

// ============================================================================
// BoxHash v2 — deterministic integrity and content hashing
// ============================================================================

static TestResult test_boxhash_determinism(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seed[16];
    BoxHashContext c1, c2;

    for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(i * 7 + 1);
    BoxHashInit(&c1, seed, 16);
    BoxHashInit(&c2, seed, 16);
    for (int i = 0; i < 256; i++) g_test_buffer[i] = (uint8_t)(i * 13 + 5);

    BoxHash h1 = BoxHashContent(g_test_buffer, 256, &c1);
    BoxHash h2 = BoxHashContent(g_test_buffer, 256, &c2);
    TEST_ASSERT(BoxHashEqual(&h1, &h2), "same data+seed -> same 256-bit content hash");

    uint64_t i1 = BoxHashIntegrity(g_test_buffer, 256, &c1);
    uint64_t i2 = BoxHashIntegrity(g_test_buffer, 256, &c2);
    TEST_ASSERT(i1 == i2, "same data+seed -> same 64-bit integrity hash");
    TEST_ASSERT(i1 != 0, "integrity hash nonzero for nonempty data");
out:
    return verdict;
}

static TestResult test_boxhash_seed_separation(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        s1[16], s2[16];
    BoxHashContext c1, c2;

    for (int i = 0; i < 16; i++) { s1[i] = (uint8_t)i; s2[i] = (uint8_t)(i + 1); }
    BoxHashInit(&c1, s1, 16);
    BoxHashInit(&c2, s2, 16);
    for (int i = 0; i < 512; i++) g_test_buffer[i] = (uint8_t)(i & 0xFF);
    BoxHash h1 = BoxHashContent(g_test_buffer, 512, &c1);
    BoxHash h2 = BoxHashContent(g_test_buffer, 512, &c2);
    TEST_ASSERT(!BoxHashEqual(&h1, &h2), "different volume seeds -> different hash");
out:
    return verdict;
}

static TestResult test_boxhash_avalanche(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seed[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    BoxHashContext c;

    BoxHashInit(&c, seed, 8);
    for (int i = 0; i < 4096; i++) g_test_buffer[i] = (uint8_t)(i * 3 + 1);
    BoxHash a = BoxHashContent(g_test_buffer, 4096, &c);
    g_test_buffer[2000] ^= 0x01;                 // flip a single bit
    BoxHash b = BoxHashContent(g_test_buffer, 4096, &c);
    TEST_ASSERT(!BoxHashEqual(&a, &b), "1-bit change -> different hash");

    int diff = 0;
    for (int i = 0; i < BOX_HASH_BYTES; i++) {
        uint8_t x = (uint8_t)(a.bytes[i] ^ b.bytes[i]);
        while (x) { diff += x & 1; x >>= 1; }
    }
    TEST_ASSERT(diff > 64, "avalanche: >64 of 256 output bits flip on a 1-bit input change");
out:
    return verdict;
}

static TestResult test_boxhash_sha256_kat(void)
{
    TestResult verdict = TEST_PASS;
    // FIPS 180-4: SHA-256("abc")
    static const uint8_t expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    BoxHash h = BoxHashSecure("abc", 3);
    TEST_ASSERT(memcmp(h.bytes, expected, 32) == 0, "SHA-256(\"abc\") known-answer vector");
out:
    return verdict;
}

static TestResult test_boxhash_sizes(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seed[4] = {9, 8, 7, 6};
    BoxHashContext c;

    BoxHashInit(&c, seed, 4);
    // Exercise every tail length class (wyhash 1-3 / 4-7 / 8-16 / >16) — no OOB.
    static const uint32_t sizes[] = {1,7,8,15,16,17,31,32,33,63,64,65,4096};
    uint64_t prev = 0;
    for (uint32_t k = 0; k < sizeof(sizes)/sizeof(sizes[0]); k++) {
        uint32_t n = sizes[k];
        for (uint32_t i = 0; i < n; i++) g_test_buffer[i] = (uint8_t)(i + n);
        uint64_t a = BoxHashIntegrity(g_test_buffer, n, &c);
        uint64_t b = BoxHashIntegrity(g_test_buffer, n, &c);
        TEST_ASSERT(a == b, "deterministic across all tail lengths");
        TEST_ASSERT(a != prev, "distinct content -> distinct integrity hash");
        prev = a;
    }
    TEST_ASSERT(BoxHashIntegrity(NULL, 0, &c) == 0, "null/empty input -> 0");
out:
    return verdict;
}

static const TestCase g_boxhash_tests[] = {
    { .name = "boxhash_determinism",     .func = test_boxhash_determinism,     .reach = TEST_READS_ONLY },
    { .name = "boxhash_seed_separation", .func = test_boxhash_seed_separation, .reach = TEST_READS_ONLY },
    { .name = "boxhash_avalanche",       .func = test_boxhash_avalanche,       .reach = TEST_READS_ONLY },
    { .name = "boxhash_sha256_kat",      .func = test_boxhash_sha256_kat,      .reach = TEST_READS_ONLY },
    { .name = "boxhash_sizes",           .func = test_boxhash_sizes,           .reach = TEST_READS_ONLY },
};

const TestSuite g_boxhash_suite = {
    .tests = g_boxhash_tests, .count = ARRAY_SIZE(g_boxhash_tests),
};
