#include "tests.h"
#include "volume_book.h"
#include "crypto.h"
#include "cpuid.h"

// ============================================================================
// The book's alphabet (volume_book.h) held to known answers — its sum, its
// numbers, its sectors, its records, its pointer and its atom
//
// Arithmetic over memory and nothing else: no medium is touched, so the suite
// is in every build, not only a stand's. It runs where the startup suites run
// — on every boot that mounts a volume, inside the volume's door (tests.c,
// TagFS_RunTests); a boot with no volume, or with one refused, runs none. The
// kernel's CRC32C takes the crc32 instruction when the processor carries
// SSE4.2 (crypto.c, KCrc32c), and the only place that instruction meets real
// silicon is a board — a desk run under -cpu max proves QEMU's crc32, not the
// part's. There every such boot holds it to the table, which is the sum's
// definition.
// ============================================================================

/* The volume every sector here belongs to: 00 01 .. 0F, the uuid the golden
 * sector is defined over (volume_book.h, BOOK_GOLDEN_SECTOR_CRC). */
static void BookTestUuid(uint8_t uuid[16])
{
    for (uint32_t i = 0; i < 16; i++) uuid[i] = (uint8_t)i;
}

/* Write a sector head into `sector` and seal it for `vlba` of `uuid` — the
 * payload is whatever the caller left in bytes 24..511. */
static void BookTestSeal(uint8_t *sector, const uint8_t uuid[16], uint64_t vlba,
                         const BookSectorHead *head)
{
    memcpy(sector, head, sizeof(*head));
    uint32_t sum = ~BookCrc32cOn(BookSectorSeed(uuid, vlba), sector + 4, BOOK_SECTOR_BYTES - 4u);
    memcpy(sector, &sum, sizeof(sum));
}

/*
 * CRC-32/ISCSI's check value and RFC 3720 B.4's five vectors, by the table and
 * by the kernel's own CRC32C — whichever way the kernel takes on this
 * processor — and the kernel's sum carried on over pieces as a sector's is.
 */
static TestResult TestCrc32cVectors(void)
{
    TestResult verdict = TEST_PASS;
    static const uint8_t pdu[48] = {
        0x01, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x14, 0, 0, 0, 0, 0, 0x04, 0, 0, 0, 0, 0x14, 0, 0, 0, 0x18,
        0x28, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0 };
    static const uint32_t want[6] = { BOOK_CRC32C_CHECK, 0x8A9136AAu, 0x62A8AB43u,
                                      0x46DD794Eu, 0x113FDB5Cu, 0xD9963A56u };
    uint8_t *v = g_test_buffer;

    for (uint32_t k = 0; k < 6; k++) {
        uint32_t n = 32;
        for (uint32_t i = 0; i < 32; i++) {
            v[i] = k == 1 ? 0x00 : k == 2 ? 0xFF : k == 3 ? (uint8_t)i : (uint8_t)(31u - i);
        }
        if (k == 0) { memcpy(v, "123456789", 9); n = 9; }
        if (k == 5) { memcpy(v, pdu, sizeof(pdu)); n = sizeof(pdu); }
        TEST_ASSERT_EQ(~BookCrc32cOn(0xFFFFFFFFu, v, n), want[k], "CRC32C by the table");
        TEST_ASSERT_EQ(KCrc32c(v, n), want[k], "CRC32C by the kernel's own way");
    }

    /* A sector is summed in three pieces (uuid, its address, its bytes): the
     * kernel's sum, carried on, is the sum of the whole. */
    for (uint32_t i = 0; i < 4096; i++) v[i] = (uint8_t)(i * 31u + 7u);
    uint32_t whole = KCrc32c(v, 4096);
    TEST_ASSERT_EQ(~BookCrc32cOn(0xFFFFFFFFu, v, 4096), whole, "four KiB by the table and the kernel");
    TEST_ASSERT_EQ(KCrc32cContinue(KCrc32cContinue(KCrc32c(v, 13), v + 13, 1000), v + 1013, 3083),
                   whole, "the kernel's sum carried on over three pieces");
    TEST_ASSERT_EQ(KCrc32cContinue(0, v, 4096), whole, "carried on from nothing is the sum");

    kprintf("[TESTS] crc32c_vectors: the kernel's CRC32C ran on %s, and agreed with the table\n",
            g_cpu_caps.has_sse42 ? "the crc32 instruction (SSE4.2)"
                                 : "the table (this processor carries no SSE4.2)");
out:
    return verdict;
}

/* Every edge of the number writing: one byte to ten, the tenth's single bit,
 * and the four ways a number can be written wrong. */
static TestResult TestVarintEdges(void)
{
    TestResult verdict = TEST_PASS;
    static const uint64_t value[9] = { 0, 1, 127, 128, 16383, 16384, 0xFFFFFFFFull,
                                       0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull };
    static const uint32_t bytes[9] = { 1, 1, 1, 2, 2, 3, 5, 10, 10 };
    uint8_t  b[12];
    uint64_t got;

    for (uint32_t k = 0; k < 9; k++) {
        TEST_ASSERT_EQ(BookVarintPut(b, value[k]), bytes[k], "a number takes the bytes it needs");
        TEST_ASSERT_EQ(BookVarintGet(b, bytes[k], &got), bytes[k], "and reads back in as many");
        TEST_ASSERT(got == value[k], "the number read back is the number written");
        TEST_ASSERT_EQ(BookVarintGet(b, bytes[k] - 1u, &got), 0, "one byte short is refused");
    }

    b[0] = 0x80; b[1] = 0x00;
    TEST_ASSERT_EQ(BookVarintGet(b, 2, &got), 0, "zero written in two bytes is refused");
    b[0] = 0xFF; b[1] = 0x80; b[2] = 0x00;
    TEST_ASSERT_EQ(BookVarintGet(b, 3, &got), 0, "a trailing zero byte is refused");
    for (uint32_t i = 0; i < 9; i++) b[i] = 0xFF;
    b[9] = 0x02;
    TEST_ASSERT_EQ(BookVarintGet(b, 10, &got), 0, "a tenth byte above 0x01 is refused");
    b[9] = 0x81; b[10] = 0x00;
    TEST_ASSERT_EQ(BookVarintGet(b, 11, &got), 0, "an eleventh byte is refused");
    b[9] = 0x01;
    TEST_ASSERT_EQ(BookVarintGet(b, 10, &got), 10, "ten bytes ending 0x01 are the largest number");
    TEST_ASSERT(got == 0xFFFFFFFFFFFFFFFFull, "and read back as 2^64-1");

    TEST_ASSERT_EQ(BookZigzag(0), 0, "zigzag 0");
    TEST_ASSERT_EQ(BookZigzag(-1), 1, "zigzag -1");
    TEST_ASSERT_EQ(BookZigzag(1), 2, "zigzag 1");
    TEST_ASSERT(BookZigzag(INT64_MIN) == 0xFFFFFFFFFFFFFFFFull, "zigzag of the least number is the largest");
    TEST_ASSERT(BookUnzigzag(BookZigzag(INT64_MIN)) == INT64_MIN, "and comes back");
    TEST_ASSERT(BookUnzigzag(BookZigzag(INT64_MAX)) == INT64_MAX, "the greatest comes back");
out:
    return verdict;
}

/*
 * A sector holds only for its own volume, at its own address, with a head a
 * writer could have written — and a unit holds only when all of its sectors
 * hold and agree. 1 to 5 flipped bits anywhere in a sector are all caught:
 * CRC32C's Hamming distance over 512 bytes is 6.
 */
static TestResult TestSectorHead(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        uuid[16], other[16];
    uint8_t       *s = g_test_buffer;
    const uint64_t vlba = 0x10008;
    BookSectorHead h = { .lead = 24, .kind = BOOK_UNIT_SHEET, .at = 0, .of = 8, .seq = 5 };

    BookTestUuid(uuid);
    BookTestUuid(other);
    other[15] ^= 0x01;
    for (uint32_t i = 0; i < 8u * BOOK_SECTOR_BYTES; i++) s[i] = (uint8_t)(i * 13u + 1u);
    BookTestSeal(s, uuid, vlba, &h);
    TEST_ASSERT(BookSectorCheck(s, uuid, vlba), "a sealed sector holds at its address");
    TEST_ASSERT(!BookSectorCheck(s, uuid, vlba + 1u), "and not at the next one");
    TEST_ASSERT(!BookSectorCheck(s, other, vlba), "and not for another volume");

    static const uint32_t place[5] = { 0, 4, 24, 300, 511 };
    for (uint32_t flips = 1; flips <= 5; flips++) {
        memcpy(g_test_output, s, BOOK_SECTOR_BYTES);
        for (uint32_t f = 0; f < flips; f++)
            g_test_output[place[f]] ^= (uint8_t)(1u << ((f * 3u + flips) & 7u));
        TEST_ASSERT_EQ(BookSectorCheck(g_test_output, uuid, vlba), 0, "flipped bits are caught");
    }

    /* Each rule of the head, broken alone under a sum that holds. */
    static const struct { uint16_t lead, kind; uint32_t at, of; uint64_t seq; } bad[7] = {
        { 24, 0, 0, 8, 5 }, { 24, 4, 0, 8, 5 }, { 24, BOOK_UNIT_SHEET, 8, 8, 5 },
        { 24, BOOK_UNIT_SHEET, 0, 8, 0 }, { 1, BOOK_UNIT_SHEET, 0, 8, 5 },
        { 23, BOOK_UNIT_SHEET, 0, 8, 5 }, { 512, BOOK_UNIT_SHEET, 0, 8, 5 } };
    for (uint32_t k = 0; k < 7; k++) {
        BookSectorHead b = { .lead = bad[k].lead, .kind = bad[k].kind, .at = bad[k].at,
                             .of = bad[k].of, .seq = bad[k].seq };
        memcpy(g_test_output, s, BOOK_SECTOR_BYTES);
        BookTestSeal(g_test_output, uuid, vlba, &b);
        TEST_ASSERT_EQ(BookSectorCheck(g_test_output, uuid, vlba), 0, "a head no writer writes");
    }
    h.lead = 0;
    BookTestSeal(g_test_output, uuid, vlba, &h);
    TEST_ASSERT(BookSectorCheck(g_test_output, uuid, vlba), "a sector only continuing a record holds");
    h.lead = 511;
    BookTestSeal(g_test_output, uuid, vlba, &h);
    TEST_ASSERT(BookSectorCheck(g_test_output, uuid, vlba), "a record beginning at its last byte holds");

    /* A unit of eight. */
    BookSectorHead got;
    for (uint32_t at = 0; at < 8; at++) {
        BookSectorHead u = { .lead = 24, .kind = BOOK_UNIT_SHEET, .at = at, .of = 8, .seq = 5 };
        BookTestSeal(s + at * BOOK_SECTOR_BYTES, uuid, vlba + at, &u);
    }
    TEST_ASSERT(BookUnitCheck(s, 8, uuid, vlba, &got), "a whole unit of eight holds");
    TEST_ASSERT(got.kind == BOOK_UNIT_SHEET && got.seq == 5, "and names its kind and seq");
    TEST_ASSERT(!BookUnitCheck(s, 7, uuid, vlba, &got), "seven of its eight are not the unit");
    TEST_ASSERT(!BookUnitCheck(s + BOOK_SECTOR_BYTES, 7, uuid, vlba + 1u, &got),
                "nor are its last seven");
    BookSectorHead late = { .lead = 24, .kind = BOOK_UNIT_SHEET, .at = 6, .of = 8, .seq = 6 };
    BookTestSeal(s + 6u * BOOK_SECTOR_BYTES, uuid, vlba + 6u, &late);
    TEST_ASSERT(!BookUnitCheck(s, 8, uuid, vlba, &got), "a sector of another seq breaks the unit");
    BookSectorHead swapped = { .lead = 24, .kind = BOOK_UNIT_SHEET, .at = 7, .of = 8, .seq = 5 };
    BookTestSeal(s + 6u * BOOK_SECTOR_BYTES, uuid, vlba + 6u, &swapped);
    TEST_ASSERT(!BookUnitCheck(s, 8, uuid, vlba, &got), "a sector out of its order breaks the unit");
    BookSectorHead six = { .lead = 24, .kind = BOOK_UNIT_SHEET, .at = 6, .of = 8, .seq = 5 };
    BookTestSeal(s + 6u * BOOK_SECTOR_BYTES, uuid, vlba + 6u, &six);
    TEST_ASSERT(BookUnitCheck(s, 8, uuid, vlba, &got), "its own sector put back, the unit holds again");
    BookSectorHead stranger = { .lead = 24, .kind = BOOK_UNIT_STITCH, .at = 6, .of = 8, .seq = 5 };
    BookTestSeal(s + 6u * BOOK_SECTOR_BYTES, uuid, vlba + 6u, &stranger);
    TEST_ASSERT(!BookUnitCheck(s, 8, uuid, vlba, &got), "a sector of another kind breaks the unit");
out:
    return verdict;
}

/* The golden sector, built by its rule in volume_book.h: its sum is the number
 * the header carries — which volume-read's self-test computes the same way —
 * by the table and by the kernel's own CRC32C. */
static TestResult TestGoldenSector(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        uuid[16];
    uint8_t       *s = g_test_buffer;
    BookSectorHead h = { .lead = 24, .kind = BOOK_UNIT_STITCH, .at = 0, .of = 8, .seq = 1 };

    BookTestUuid(uuid);
    for (uint32_t i = 0; i < BOOK_SECTOR_BYTES; i++) s[i] = (uint8_t)(i * 7u + 3u);
    BookTestSeal(s, uuid, 0x2000, &h);
    uint32_t sum;
    memcpy(&sum, s, sizeof(sum));
    TEST_ASSERT_EQ(sum, BOOK_GOLDEN_SECTOR_CRC, "the golden sector's sum by the table");
    TEST_ASSERT_EQ(KCrc32cContinue(~BookSectorSeed(uuid, 0x2000), s + 4, BOOK_SECTOR_BYTES - 4u),
                   BOOK_GOLDEN_SECTOR_CRC, "the golden sector's sum by the kernel's own CRC32C");
    TEST_ASSERT(BookSectorCheck(s, uuid, 0x2000), "the golden sector holds");
out:
    return verdict;
}

/* Append one record to a stream being built: tag, length, body of `len`
 * bytes of `fill`. Returns where the next record goes. */
static uint32_t BookTestRecord(uint8_t *stream, uint32_t at, uint64_t tag, uint32_t len, uint8_t fill)
{
    at += BookVarintPut(stream + at, tag);
    at += BookVarintPut(stream + at, len);
    for (uint32_t i = 0; i < len; i++) stream[at + i] = fill;
    return at + len;
}

/*
 * Records across a sector's edge: a unit of eight sectors is written from one
 * stream, read back into another, and walked — every record found whole, its
 * tag handed over with the ignorable bit, PAD the end; the walk taken up again
 * from a sector's `lead`, as after a sector before it is lost; a length past
 * the stream's end refused before anything is taken from it.
 */
static TestResult TestEntryWalk(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    uuid[16];
    uint8_t   *flat = g_test_output;
    uint8_t   *unit = g_test_buffer;
    uint32_t   at = 0, body = 0, len = 0, walked = 0, begins[4];
    uint64_t   tag = 0;
    static const uint32_t size[4] = { 3, 700, 0, 2000 };
    static const uint64_t tags[4] = { 1u << 1, (31u << 1) | 1u, 4u << 1, 300u << 1 };

    BookTestUuid(uuid);
    memset(flat, 0, 8u * BOOK_PAYLOAD_BYTES);
    for (uint32_t k = 0; k < 4; k++) {
        begins[k] = at;
        at = BookTestRecord(flat, at, tags[k], size[k], (uint8_t)(k + 1u));
    }
    for (uint32_t s = 0; s < 8; s++) {
        BookSectorHead h = { .lead = 0, .kind = BOOK_UNIT_SLOT, .at = s, .of = 8, .seq = 9 };
        for (uint32_t k = 4; k-- > 0; )
            if (begins[k] / BOOK_PAYLOAD_BYTES == s)
                h.lead = (uint16_t)(BOOK_HEAD_BYTES + begins[k] % BOOK_PAYLOAD_BYTES);
        memcpy(unit + s * BOOK_SECTOR_BYTES + BOOK_HEAD_BYTES, flat + s * BOOK_PAYLOAD_BYTES,
               BOOK_PAYLOAD_BYTES);
        BookTestSeal(unit + s * BOOK_SECTOR_BYTES, uuid, 0x800u + s, &h);
    }
    TEST_ASSERT(BookUnitCheck(unit, 8, uuid, 0x800, NULL), "the unit holds");
    memset(flat, 0xEE, 8u * BOOK_PAYLOAD_BYTES);
    TEST_ASSERT_EQ(BookUnitStream(unit, 8, flat), 8u * BOOK_PAYLOAD_BYTES, "the stream is 8 x 488 bytes");

    uint32_t room = 8u * BOOK_PAYLOAD_BYTES;
    at = 0;
    int said;
    while ((said = BookEntryNext(flat, room, &at, &tag, &body, &len)) == 1) {
        TEST_ASSERT(walked < 4, "no record past the four written");
        TEST_ASSERT(tag == tags[walked] && len == size[walked], "each record as it was written");
        for (uint32_t i = 0; i < len; i++)
            TEST_ASSERT_EQ(flat[body + i], walked + 1u, "its body whole across the sectors' edges");
        walked++;
    }
    TEST_ASSERT_EQ(said, 0, "PAD ends the walk");
    TEST_ASSERT_EQ(walked, 4, "all four records walked");

    /* Records 2 and 3 begin in sector 1, 708 and 710 bytes into the stream,
     * behind the body of record 1 that began in sector 0. A reader that lost
     * sector 0 finds them by sector 1's `lead` alone; sector 2 begins nothing. */
    BookSectorHead s1, s2;
    memcpy(&s1, unit + 1u * BOOK_SECTOR_BYTES, sizeof(s1));
    memcpy(&s2, unit + 2u * BOOK_SECTOR_BYTES, sizeof(s2));
    TEST_ASSERT_EQ(s2.lead, 0, "a sector that only continues a record leads nowhere");
    at = 1u * BOOK_PAYLOAD_BYTES + (uint32_t)(s1.lead - BOOK_HEAD_BYTES);
    TEST_ASSERT_EQ(BookEntryNext(flat, room, &at, &tag, &body, &len), 1, "a walk taken up at a lead");
    TEST_ASSERT(tag == tags[2] && len == size[2], "finds the first record that begins there");
    TEST_ASSERT_EQ(BookEntryNext(flat, room, &at, &tag, &body, &len), 1, "and walks on");
    TEST_ASSERT(tag == tags[3] && len == size[3], "to the next");

    /* A record ending on the stream's last byte is whole; one byte less of
     * stream and its length runs past the end — refused, with nothing taken. */
    uint32_t end = BookTestRecord(flat, 0, 2u << 1, 50, 0x55);
    at = 0;
    TEST_ASSERT_EQ(BookEntryNext(flat, end, &at, &tag, &body, &len), 1,
                   "a record ending on the stream's last byte is whole");
    TEST_ASSERT(at == end && body == 2u && len == 50u, "and the walk stands at the stream's end");
    at = 0;
    TEST_ASSERT_EQ(BookEntryNext(flat, end - 1u, &at, &tag, &body, &len), -1,
                   "a length one byte past the stream's end is refused before its body is read");
    TEST_ASSERT_EQ(at, 0, "and the walk does not move");
    flat[0] = 0x80;
    TEST_ASSERT_EQ(BookEntryNext(flat, 1, &at, &tag, &body, &len), -1, "a tag cut short is refused");
    at = 52;
    TEST_ASSERT_EQ(BookEntryNext(flat, 52, &at, &tag, &body, &len), 0, "the stream's end ends the walk");
out:
    return verdict;
}

/* The pointer's copies are read in their order and as many as it says; a
 * pointer that could name no node is refused. */
static TestResult TestBookPtr(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        b[64];
    BookPtr        p;
    uint64_t       block = 0;
    const uint64_t blocks[3] = { 1024, 0x123456789ull, 7 };

    uint32_t n = BookPtrPut(b, 77, 2, blocks, 3);
    TEST_ASSERT(n > 0, "a pointer of three copies is written");
    TEST_ASSERT_EQ(BookPtrGet(b, n, &p), n, "and read back in as many bytes");
    TEST_ASSERT(p.seq == 77 && p.sheets == 2 && p.copies == 3, "seq, sheets and copies as written");
    uint32_t at = 0;
    for (uint32_t k = 0; k < 3; k++) {
        at += BookVarintGet(p.blocks + at, p.blocks_bytes - at, &block);
        TEST_ASSERT(block == blocks[k], "each copy's block in its order");
    }
    TEST_ASSERT_EQ(at, p.blocks_bytes, "and nothing after the last");
    TEST_ASSERT_EQ(BookPtrGet(b, n - 1u, &p), 0, "a pointer cut short is refused");

    TEST_ASSERT_EQ(BookPtrPut(b, 0, 1, blocks, 1), 0, "a pointer to seq 0 is not written");
    TEST_ASSERT_EQ(BookPtrPut(b, 1, 0, blocks, 1), 0, "nor one of no sheets");
    TEST_ASSERT_EQ(BookPtrPut(b, 1, 1, blocks, 0), 0, "nor one of no copies");
    static const uint8_t wrong[4][5] = { { 0, 1, 1, 8, 0 }, { 3, 0, 1, 8, 0 },
                                         { 3, 1, 0, 8, 0 }, { 3, 1, 5, 8, 9 } };
    for (uint32_t k = 0; k < 4; k++)
        TEST_ASSERT_EQ(BookPtrGet(wrong[k], 5, &p), 0, "seq 0, no sheets, no copies, copies past the end");

    /* The widest node there is: as many Sheets as a unit's `of` counts
     * sectors in, its one copy ending on the last sector a vlba can name. One
     * Sheet more, or a copy one block further on, names no unit at all. */
    const uint64_t most = 0xFFFFFFFFu / VOLUME_BLOCK_SECTORS;
    const uint64_t top  = ((uint64_t)1 << 61) - most;
    const uint64_t past = top + 1u;
    n = BookPtrPut(b, 3, most, &top, 1);
    TEST_ASSERT(n > 0, "the widest node, ending on the last sector there is, is written");
    TEST_ASSERT_EQ(BookPtrGet(b, n, &p), n, "and read back");
    TEST_ASSERT(p.sheets == most, "with all its sheets");
    TEST_ASSERT_EQ(BookPtrPut(b, 3, most + 1u, blocks, 1), 0, "one Sheet more is not written");
    TEST_ASSERT_EQ(BookPtrPut(b, 3, most, &past, 1), 0, "nor a copy one block further on");
    n = BookVarintPut(b, 3);
    n += BookVarintPut(b + n, most + 1u);
    n += BookVarintPut(b + n, 1);
    n += BookVarintPut(b + n, 8);
    TEST_ASSERT_EQ(BookPtrGet(b, n, &p), 0, "and one Sheet more is refused");
    n = BookVarintPut(b, 3);
    n += BookVarintPut(b + n, most);
    n += BookVarintPut(b + n, 2);
    n += BookVarintPut(b + n, 8);
    n += BookVarintPut(b + n, past);
    TEST_ASSERT_EQ(BookPtrGet(b, n, &p), 0, "as is a copy whose last sector no vlba can name");
out:
    return verdict;
}

/* The atom, and whether a ground starts on one: vectors of volume_book.h's
 * rule, including the offset an ATA disk with an "XP jumper" names. */
static TestResult TestGroundAtom(void)
{
    TestResult verdict = TEST_PASS;

    TEST_ASSERT_EQ(BookAtomOf(4096, 512, 512), 4096, "a 512e medium's atom is 4 KiB");
    TEST_ASSERT_EQ(BookAtomOf(0, 0, 512), 4096, "4 KiB is the floor");
    TEST_ASSERT_EQ(BookAtomOf(4096, 8192, 512), 8192, "a coarser physical block is the atom");
    TEST_ASSERT_EQ(BookAtomOf(4096, 4096, 16384), 16384, "and so is a coarser logical one");
    TEST_ASSERT(BookGroundOnAtom(2048, 0, 4096), "sector 2048 starts a 4 KiB block");
    TEST_ASSERT(!BookGroundOnAtom(2049, 0, 4096), "sector 2049 does not");
    TEST_ASSERT(!BookGroundOnAtom(63, 0, 4096), "sector 63 does not on an aligned medium");
    TEST_ASSERT(BookGroundOnAtom(63, 7, 4096), "but does where the medium's blocks start at 7");
    TEST_ASSERT(!BookGroundOnAtom(3, 7, 4096), "a ground inside the medium's first, partial block does not");
    TEST_ASSERT(BookGroundOnAtom(2048, 0, 8192), "an 8 KiB atom starts at sector 2048");
    TEST_ASSERT(!BookGroundOnAtom(2056, 0, 8192), "and not eight sectors on");
out:
    return verdict;
}

static const TestCase g_book_tests[] = {
    { .name = "crc32c_vectors", .func = TestCrc32cVectors, .reach = TEST_READS_ONLY },
    { .name = "varint_edges",   .func = TestVarintEdges,   .reach = TEST_READS_ONLY },
    { .name = "sector_head",    .func = TestSectorHead,    .reach = TEST_READS_ONLY },
    { .name = "golden_sector",  .func = TestGoldenSector,  .reach = TEST_READS_ONLY },
    { .name = "entry_walk",     .func = TestEntryWalk,     .reach = TEST_READS_ONLY },
    { .name = "book_ptr",       .func = TestBookPtr,       .reach = TEST_READS_ONLY },
    { .name = "ground_atom",    .func = TestGroundAtom,    .reach = TEST_READS_ONLY },
};

const TestSuite g_book_suite = {
    .tests = g_book_tests, .count = ARRAY_SIZE(g_book_tests),
};
