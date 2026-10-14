#include "tests.h"
#include "volume_book.h"
#include "volume_tally.h"
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

// ============================================================================
// The Tally's nodes and the Spine's slots (volume_tally.h): written by the
// codec, sealed, held to the golden sums volume-read reaches by its own code
// (self-test X3), and read back field by field
// ============================================================================

/* What the golden node's files wear, and the extension its last run carries
 * (volume_tally.h, TALLY_GOLDEN_NODE_CRC). */
static const uint32_t g_golden_tags_one[3]   = { 3, 5, 300 };
static const uint32_t g_golden_tags_three[1] = { 0 };
static const uint8_t  g_golden_ext[3]        = { 0x02, 0x01, 0x07 };
static const TallyKey g_golden_keys[5]       = { { 1, 0 }, { 1, 1 }, { 2, 0 }, { 3, 0 }, { 3, 1 } };

/* The golden node's Parcels: fid 1's one run, and fid 3's hole, run and run
 * that carries an extension, born at 0 — so its birth_delta is 1. */
static const TallyRun g_golden_one[1]   = { { .start = 1024, .count = 2, .birth = 1 } };
static const TallyRun g_golden_three[3] = {
    { .hole = 1, .count = 1, .birth = 1 },
    { .start = 4096, .count = 1, .birth = 1 },
    { .start = 10, .count = 1, .birth = 0, .ext = g_golden_ext, .ext_len = 3 } };

/* A FILES header into `out`, its tags written as the codec writes a list. */
static uint32_t TallyTestHead(uint8_t *out, uint64_t gen, uint64_t flags, uint64_t size, uint64_t created,
                              uint64_t modified, const char *name, const uint32_t *tags, uint32_t ntags)
{
    uint8_t       ids[40];
    TallyFileHead h = { .gen = gen, .flags = flags, .size = size, .created_ns = created,
                        .modified_ns = modified, .name = (const uint8_t *)name, .name_len = strlen(name),
                        .ntags = ntags, .tags = ids };
    h.tags_bytes = TallyIdsPut(ids, tags, ntags);
    return TallyFileHeadPut(out, &h);
}

/* The golden node's stream (volume_tally.h's rule) into `stream`, and where
 * its six records begin — the NodeHead and five items — into `starts`; the
 * stream's length. `scratch` holds each body as it is written. */
static uint32_t TallyTestGoldenNode(uint8_t *stream, uint32_t *starts, uint8_t *scratch)
{
    NodeHead head = { .tree = TALLY_FILES, .level = 0, .items = 5, .first = g_golden_keys[0] };
    uint32_t at   = NodeHeadPut(stream, &head);

    starts[0] = 0;
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t len;
        if (i == 0)
            len = TallyTestHead(scratch, 1, TALLY_FILE_ACTIVE, 5000, 1700000000000000000ull,
                                1700000000000000007ull, "golden", g_golden_tags_one, 3);
        else if (i == 1)
            len = TallyParcelPut(scratch, g_golden_one, 1, 1);
        else if (i == 2)
            len = TallyTestHead(scratch, 9, TALLY_FILE_ACTIVE | TAGFS_FILE_HIDDEN, 0, 1, 0, "", NULL, 0);
        else if (i == 3)
            len = TallyTestHead(scratch, 1, TALLY_FILE_ACTIVE, 9000, 2, 2, "three", g_golden_tags_three, 1);
        else
            len = TallyParcelPut(scratch, g_golden_three, 3, 1);
        starts[i + 1] = at;
        at += TallyItemPut(stream + at, TALLY_FILES, g_golden_keys[i], g_golden_keys[i ? i - 1 : 0],
                           scratch, len);
    }
    return at;
}

/* The golden node built and sealed into g_test_buffer, its stream left in
 * g_test_output: 1 when it was laid down, 0 when it did not fit its Sheet. */
static int TallyTestGoldenUnit(const uint8_t uuid[16])
{
    uint32_t starts[6];
    memset(g_test_output, 0, TALLY_SHEET_PAYLOAD);
    uint32_t len = TallyTestGoldenNode(g_test_output, starts, g_test_output + 8192);
    if (len > TALLY_SHEET_PAYLOAD) return 0;
    TallyUnitSeal(g_test_buffer, g_test_output, len, starts, 6, BOOK_UNIT_SHEET, 1,
                  0x2000ull * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, uuid);
    return 1;
}

/* The golden node, sealed by its rule: the sum of its eight sectors is the
 * number volume_tally.h carries, which volume-read's self-test reaches by
 * tally.py's own encoders — by the table and by the kernel's CRC32C. */
static TestResult TestGoldenNode(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        uuid[16];
    BookSectorHead head;

    BookTestUuid(uuid);
    TEST_ASSERT(TallyTestGoldenUnit(uuid), "the golden node fits one Sheet");
    TEST_ASSERT_EQ(~BookCrc32cOn(0xFFFFFFFFu, g_test_buffer, VOLUME_BLOCK_BYTES), TALLY_GOLDEN_NODE_CRC,
                   "the golden node's sum by the table");
    TEST_ASSERT_EQ(KCrc32c(g_test_buffer, VOLUME_BLOCK_BYTES), TALLY_GOLDEN_NODE_CRC,
                   "the golden node's sum by the kernel's own CRC32C");
    TEST_ASSERT(BookUnitCheck(g_test_buffer, VOLUME_BLOCK_SECTORS, uuid, 0x2000ull * VOLUME_BLOCK_SECTORS, &head),
                "it holds as one unit at its block");
    TEST_ASSERT(head.kind == BOOK_UNIT_SHEET && head.seq == 1 && head.lead == BOOK_HEAD_BYTES,
                "a SHEET of seq 1 whose first sector leads with the NodeHead");
out:
    return verdict;
}

/* Does the Parcel body at `b` read as `n` runs equal to `want`, in a node of
 * `seq`, and end where its last run does? */
static int TallyTestParcelIs(const uint8_t *b, uint32_t len, const TallyRun *want, uint32_t n, uint64_t seq)
{
    uint64_t count = 0, cursor = 0;
    uint32_t at = BookVarintGet(b, len, &count);
    if (!at || count != n) return 0;
    for (uint32_t i = 0; i < n; i++) {
        TallyRun r;
        if (!TallyRunGet(b, len, &at, &cursor, seq, &r)) return 0;
        if (r.hole != want[i].hole || r.count != want[i].count || r.birth != want[i].birth ||
            r.ext_len != want[i].ext_len || (!r.hole && r.start != want[i].start))
            return 0;
        for (uint32_t k = 0; k < r.ext_len; k++)
            if (r.ext[k] != want[i].ext[k]) return 0;
    }
    return at == len;
}

/* Does the header body at `b` read back as the golden file it was written
 * from — its numbers, its name, and each of its tags in order? */
static int TallyTestHeadIs(const uint8_t *b, uint32_t len, uint64_t gen, uint64_t flags, uint64_t size,
                           uint64_t created, uint64_t modified, const char *name,
                           const uint32_t *tags, uint32_t ntags)
{
    TallyFileHead h;
    uint32_t      at = 0;
    uint64_t      id = 0;
    if (TallyFileHeadGet(b, len, &h) != TALLY_BODY_KNOWN) return 0;
    if (h.gen != gen || h.flags != flags || h.size != size || h.created_ns != created ||
        h.modified_ns != modified || h.name_len != strlen(name) || h.ntags != ntags)
        return 0;
    for (uint32_t i = 0; i < h.name_len; i++)
        if (h.name[i] != (uint8_t)name[i]) return 0;
    for (uint32_t k = 0; k < ntags; k++)
        if (!TallyIdNext(h.tags, h.tags_bytes, &at, &id, k == 0) || id != tags[k]) return 0;
    return at == h.tags_bytes;
}

/* The golden node read back as a mount reads one: the unit, its NodeHead,
 * every item's key from the one before it, every body as it was written,
 * and PAD after the last. */
static TestResult TestTallyNodeCircle(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    uuid[16];
    NodeHead   h;
    TallyKey   key, prev;
    uint32_t   at = 0, body = 0, len = 0;
    uint8_t   *stream = g_test_output;

    BookTestUuid(uuid);
    TEST_ASSERT(TallyTestGoldenUnit(uuid), "the golden node is laid down");
    memset(stream, 0xEE, TALLY_SHEET_PAYLOAD);
    BookUnitStream(g_test_buffer, VOLUME_BLOCK_SECTORS, stream);
    TEST_ASSERT_EQ(NodeHeadGet(stream, TALLY_SHEET_PAYLOAD, &at, &h), TALLY_BODY_KNOWN, "its NodeHead reads");
    TEST_ASSERT(h.tree == TALLY_FILES && h.level == 0 && h.items == 5 && h.first.k1 == 1 && h.first.k2 == 0,
                "a FILES leaf of five items, its first key (1, 0)");
    prev = h.first;
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT(TallyItemGet(stream, TALLY_SHEET_PAYLOAD, &at, h.tree, prev, &key, &body, &len),
                    "each item reads");
        TEST_ASSERT(key.k1 == g_golden_keys[i].k1 && key.k2 == g_golden_keys[i].k2,
                    "its key, from the key before it, is the key written");
        prev = key;
        const uint8_t *b = stream + body;
        if (i == 0)
            TEST_ASSERT(TallyTestHeadIs(b, len, 1, TALLY_FILE_ACTIVE, 5000, 1700000000000000000ull,
                                        1700000000000000007ull, "golden", g_golden_tags_one, 3),
                        "fid 1's header, its three tags in order");
        else if (i == 1)
            TEST_ASSERT(TallyTestParcelIs(b, len, g_golden_one, 1, 1), "fid 1's Parcel");
        else if (i == 2)
            TEST_ASSERT(TallyTestHeadIs(b, len, 9, TALLY_FILE_ACTIVE | TAGFS_FILE_HIDDEN, 0, 1, 0, "", NULL, 0),
                        "fid 2's header: hidden, empty, no name, modified before it was made");
        else if (i == 3)
            TEST_ASSERT(TallyTestHeadIs(b, len, 1, TALLY_FILE_ACTIVE, 9000, 2, 2, "three",
                                        g_golden_tags_three, 1), "fid 3's header");
        else
            TEST_ASSERT(TallyTestParcelIs(b, len, g_golden_three, 3, 1),
                        "fid 3's Parcel: a hole, a run, and a run born at 0 with its extension byte for byte");
    }
    TEST_ASSERT_EQ(stream[at], 0, "PAD follows the last item");
out:
    return verdict;
}

/* The other bodies a leaf or an inner node carries, each written and read
 * back; a body one byte longer than this reader knows is told apart from a
 * broken one; a header not ACTIVE and a run of no blocks do not read (the
 * codec's other refusals are tally_refusals'). */
static TestResult TestTallyElements(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t       *b = g_test_output;
    TallyTag       t, got;
    BookPtr        p;
    const uint8_t *text = NULL, *d = NULL;
    uint64_t       n = 0, count = 0, blocks[2] = { 0x123456789ull, 7 }, block = 0;
    static const uint64_t digests[3] = { 0, 0x0911A376FAC2E968ull, 0xFFFFFFFFFFFFFFFFull };

    t = (TallyTag){ .flags = TALLY_TAG_HAS_VALUE, .key = (const uint8_t *)"type", .key_len = 4,
                    .value = (const uint8_t *)"elf", .value_len = 3 };
    uint32_t len = TallyTagPut(b, &t);
    TEST_ASSERT_EQ(TallyTagGet(b, len, &got), TALLY_BODY_KNOWN, "a tag with a value reads back");
    TEST_ASSERT(got.key_len == 4 && got.value_len == 3 && got.key[3] == 'e' && got.value[2] == 'f',
                "its key and its value");
    b[len] = 0;
    TEST_ASSERT_EQ(TallyTagGet(b, len + 1u, &got), TALLY_BODY_LONGER, "a byte more is a kernel that knows more");
    TEST_ASSERT_EQ(TallyTagGet(b, len - 1u, &got), TALLY_BODY_BROKEN, "a byte less is broken");
    t = (TallyTag){ .flags = TALLY_TAG_RETIRED };
    len = TallyTagPut(b, &t);
    TEST_ASSERT(len == 1u && TallyTagGet(b, len, &got) == TALLY_BODY_KNOWN && got.key_len == 0,
                "a RETIRED tag is its flags and no text");

    len = TallyUsePut(b, (const uint8_t *)"tag:a,tag:b", 11);
    TEST_ASSERT(TallyUseGet(b, len, &text, &n) == TALLY_BODY_KNOWN && n == 11 && text[10] == 'b',
                "the Use Context reads back");
    len = TallyDigestsPut(b, digests, 3);
    TEST_ASSERT(TallyDigestsGet(b, len, &n, &d) == TALLY_BODY_KNOWN && n == 3, "three digests read back");
    for (uint32_t k = 0; k < 3; k++)
        TEST_ASSERT(TallyDigestAt(d, k) == digests[k], "each one, little-endian, whole");
    uint8_t ptr[40];
    uint32_t plen = BookPtrPut(ptr, 4, 3, blocks, 2);
    len = TallyInnerPut(b, 485, ptr, plen);
    TEST_ASSERT(TallyInnerGet(b, len, &count, &p) == TALLY_BODY_KNOWN && count == 485 && p.seq == 4 &&
                p.sheets == 3 && p.copies == 2, "an inner item: its count and its child's pointer");
    TEST_ASSERT(BookVarintGet(p.blocks, p.blocks_bytes, &block) && block == blocks[0], "the first copy first");

    uint32_t ids[2] = { 7, 9 };
    TallyFileHead h = { .gen = 1, .flags = 0, .size = 1, .name = (const uint8_t *)"x", .name_len = 1,
                        .ntags = 2, .tags = ptr };
    h.tags_bytes = TallyIdsPut(ptr, ids, 2);
    len = TallyFileHeadPut(b, &h);
    TEST_ASSERT_EQ(TallyFileHeadGet(b, len, &h), TALLY_BODY_BROKEN, "a header that is not ACTIVE does not read");
    static const uint8_t empty_run[3] = { 1, 0x00, 0x00 };
    uint64_t cursor = 0;
    uint32_t at = 1;
    TallyRun r;
    TEST_ASSERT_EQ(TallyRunGet(empty_run, 3, &at, &cursor, 1, &r), 0, "a run of no blocks is refused");
out:
    return verdict;
}

/*
 * Every other rule by which the codec refuses what the book never writes
 * (§2.8-§2.9), each from bytes laid by hand that break that rule alone — the
 * bytes beside them, one number less, read. A mount stands on these: a rule
 * that stopped refusing would walk a rotten body past its bounds or past
 * 2^64, and nothing else here would say so.
 */
static TestResult TestTallyRefusals(void)
{
    TestResult     verdict = TEST_PASS;
    const uint64_t most = ~(uint64_t)0;
    TallyRun       r;
    TallyKey       key;
    NodeHead       h;
    const uint8_t *d = NULL;
    uint64_t       cursor, id, n = 0;
    uint32_t       at, body, len;
    /* A DATA run of one block is 04, then zigzag(start - cursor), then
     * birth_delta; five blocks are 14. */
    static const uint8_t below[3] = { 0x04, 0x13, 0x00 }, past[3] = { 0x04, 0x14, 0x00 },
                         over[3]  = { 0x14, 0x00, 0x00 }, early[3] = { 0x04, 0x00, 0x02 },
                         born0[3] = { 0x04, 0x00, 0x01 }, step2[2] = { 0x02, 0x01 }, pair[2] = { 0x00, 0x01 },
                         item[6]  = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t node2[6] = { 0x04, 0x04, 0x02, 0x00, 0x01, 0x00 },
                         node1[6] = { 0x02, 0x04, 0x02, 0x00, 0x01, 0x00 },
                         digests[17] = { 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

    cursor = 5, at = 0;
    TEST_ASSERT_EQ(TallyRunGet(below, 3, &at, &cursor, 1, &r), 0, "a run starting below block 0 is refused");
    cursor = 10, at = 0;
    TEST_ASSERT(TallyRunGet(below, 3, &at, &cursor, 1, &r) && r.start == 0, "ten back from block 10 reads");
    cursor = most - 4u, at = 0;
    TEST_ASSERT_EQ(TallyRunGet(past, 3, &at, &cursor, 1, &r), 0, "a run starting past 2^64 - 1 is refused");
    cursor = most - 14u, at = 0;
    TEST_ASSERT(TallyRunGet(past, 3, &at, &cursor, 1, &r) && r.start == most - 4u, "ten blocks short of it reads");
    cursor = most - 2u, at = 0;
    TEST_ASSERT_EQ(TallyRunGet(over, 3, &at, &cursor, 1, &r), 0, "a run ending past 2^64 - 1 is refused");
    cursor = 0, at = 0;
    TEST_ASSERT_EQ(TallyRunGet(early, 3, &at, &cursor, 1, &r), 0, "a run born before seq 0 is refused");
    cursor = 0, at = 0;
    TEST_ASSERT(TallyRunGet(born0, 3, &at, &cursor, 1, &r) && r.birth == 0, "born at seq 0 reads");

    at = 0;
    TEST_ASSERT_EQ(TallyKeyGet(step2, 1, &at, TALLY_TAGS, (TallyKey){ most - 1u, 0 }, &key), 0,
                   "a key stepping past 2^64 - 1 is refused");
    at = 1;
    TEST_ASSERT(TallyKeyGet(step2, 2, &at, TALLY_TAGS, (TallyKey){ most - 1u, 0 }, &key) && key.k1 == most,
                "a step to 2^64 - 1 reads");
    at = 0;
    TEST_ASSERT_EQ(TallyKeyGet(pair, 2, &at, TALLY_FILES, (TallyKey){ 5, most }, &key), 0,
                   "a part stepping past 2^64 - 1 is refused");
    id = most - 2u, at = 0;
    TEST_ASSERT_EQ(TallyIdNext(step2, 1, &at, &id, 0), 0, "an id past 2^64 - 1 is refused");
    id = most - 3u, at = 0;
    TEST_ASSERT(TallyIdNext(step2, 1, &at, &id, 0) && id == most, "an id of 2^64 - 1 reads");

    at = 0;
    TEST_ASSERT_EQ(TallyItemGet(item, 3, &at, TALLY_TAGS, (TallyKey){ 0, 0 }, &key, &body, &len), 0,
                   "an item whose length runs past the stream is refused");
    TEST_ASSERT_EQ(at, 0, "and the walk is left where it was");
    TEST_ASSERT(TallyItemGet(item, 6, &at, TALLY_TAGS, (TallyKey){ 0, 0 }, &key, &body, &len) && len == 4,
                "in a stream long enough it reads");
    at = 0;
    TEST_ASSERT_EQ(NodeHeadGet(node2, 6, &at, &h), TALLY_BODY_BROKEN, "a stream that opens on a record not "
                   "a NodeHead is no node");
    TEST_ASSERT(NodeHeadGet(node1, 6, &at, &h) == TALLY_BODY_KNOWN && h.tree == TALLY_TAGS && h.items == 1,
                "the same body as a NodeHead reads");
    TEST_ASSERT_EQ(TallyDigestsGet(digests, 16, &n, &d), TALLY_BODY_BROKEN,
                   "two digests in a body a byte short of them are refused");
    TEST_ASSERT(TallyDigestsGet(digests, 17, &n, &d) == TALLY_BODY_KNOWN && n == 2 && d == digests + 1,
                "and in a body of their length they read");
out:
    return verdict;
}

/* DIGEST_ITEM_MOST is the most digests one element can carry and still share
 * a Sheet with its node's NodeHead, whatever the numbers: 485 fit, 486 do
 * not (volume_tally.h; spec_v2 §16.3 п.1). */
static TestResult TestDigestItemMost(void)
{
    TestResult verdict = TEST_PASS;
    TallyKey   far = { 0xFFFFFFFFFFFFFFFFull, 0 };
    NodeHead   h   = { .tree = TALLY_DIGESTS, .level = 0, .items = 1, .first = far };
    uint32_t   head = NodeHeadBytes(&h);

    TEST_ASSERT_EQ(head, 15, "a leaf's NodeHead of one item and the widest key takes 15 bytes");
    TEST_ASSERT(head + TallyItemBytes(TALLY_DIGESTS, far, far, TallyVarintBytes(DIGEST_ITEM_MOST) +
                                      8u * DIGEST_ITEM_MOST) <= TALLY_SHEET_PAYLOAD,
                "DIGEST_ITEM_MOST digests fit a Sheet beside it");
    TEST_ASSERT(head + TallyItemBytes(TALLY_DIGESTS, far, far, TallyVarintBytes(DIGEST_ITEM_MOST + 1u) +
                                      8u * (DIGEST_ITEM_MOST + 1u)) > TALLY_SHEET_PAYLOAD,
                "and one more does not");
out:
    return verdict;
}

/* The golden slot's stream (volume_tally.h's rule) into `stream`, where its
 * seven records begin into `starts`; the stream's length. */
static uint32_t TallyTestGoldenSlot(uint8_t *stream, uint32_t *starts)
{
    static const uint64_t files[2] = { 2048, 2304 }, tags[2] = { 2049, 2305 }, digests[2] = { 1500, 1756 };
    SpineHead   head   = { .tally_seq = 5, .stitch_seq = 9, .stitch_clasp = 0xC0FFEE11u,
                           .made_ns = 1700000000123456789ull };
    SpineRibbon ribbon = { .at = 2600, .quire_block = 2560, .quire_blocks = 256, .next_block = 2816,
                           .next_blocks = 256 };
    SpineCounts counts = { .next_fid = 71, .next_tag = 52, .next_bookmark = 1 };
    uint8_t     ptr[40];
    uint32_t    at = 0, p;

    starts[0] = at;
    at += SpineHeadPut(stream + at, &head);
    p = BookPtrPut(ptr, 5, 1, files, 2);
    starts[1] = at;
    at += SpineRootPut(stream + at, TALLY_FILES, 0, 1, 70, ptr, p);
    p = BookPtrPut(ptr, 5, 1, tags, 2);
    starts[2] = at;
    at += SpineRootPut(stream + at, TALLY_TAGS, 0, 0, 40, ptr, p);
    p = BookPtrPut(ptr, 3, 2, digests, 2);
    starts[3] = at;
    at += SpineRootPut(stream + at, TALLY_DIGESTS, 0, 1, 15000, ptr, p);
    starts[4] = at;
    at += SpineRibbonPut(stream + at, &ribbon);
    starts[5] = at;
    at += SpineCountsPut(stream + at, &counts);
    starts[6] = at;
    at += SpineFlagsPut(stream + at, SPINE_MEDIUM_LIED);
    return at;
}

/* The golden slot built and sealed into g_test_buffer at slot 0's block. */
static void TallyTestGoldenSlotUnit(const uint8_t uuid[16])
{
    uint32_t starts[7];
    memset(g_test_output, 0, TALLY_SHEET_PAYLOAD);
    uint32_t len = TallyTestGoldenSlot(g_test_output, starts);
    TallyUnitSeal(g_test_buffer, g_test_output, len, starts, 7, BOOK_UNIT_SLOT, 2,
                  256ull * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, uuid);
}

/* The golden slot, sealed by its rule: its sum is volume_tally.h's number,
 * which self-test reaches by tally.py's own slot writer. */
static TestResult TestGoldenSlot(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        uuid[16];
    BookSectorHead head;

    BookTestUuid(uuid);
    TallyTestGoldenSlotUnit(uuid);
    TEST_ASSERT_EQ(~BookCrc32cOn(0xFFFFFFFFu, g_test_buffer, VOLUME_BLOCK_BYTES), TALLY_GOLDEN_SLOT_CRC,
                   "the golden slot's sum by the table");
    TEST_ASSERT_EQ(KCrc32c(g_test_buffer, VOLUME_BLOCK_BYTES), TALLY_GOLDEN_SLOT_CRC,
                   "the golden slot's sum by the kernel's own CRC32C");
    TEST_ASSERT(BookUnitCheck(g_test_buffer, VOLUME_BLOCK_SECTORS, uuid, 256ull * VOLUME_BLOCK_SECTORS, &head) &&
                head.kind == BOOK_UNIT_SLOT && head.seq == 2, "a SLOT of seq 2 that holds at slot 0's block");
out:
    return verdict;
}

/* One SpineRoot of the golden slot read back: its tree, level and count, and
 * its pointer's seq, Sheets and two copies. */
static int TallyTestRootIs(const uint8_t *b, uint32_t len, uint64_t tree, uint64_t level, uint64_t items,
                           uint64_t seq, uint64_t sheets, uint64_t one, uint64_t two)
{
    SpineRoot r;
    uint64_t  block = 0;
    uint32_t  at;
    if (SpineRootGet(b, len, &r) != TALLY_BODY_KNOWN) return 0;
    if (r.tree != tree || r.flags != 0 || r.level != level || r.items != items || r.ptr.seq != seq ||
        r.ptr.sheets != sheets || r.ptr.copies != 2)
        return 0;
    if (!(at = BookVarintGet(r.ptr.blocks, r.ptr.blocks_bytes, &block)) || block != one) return 0;
    return BookVarintGet(r.ptr.blocks + at, r.ptr.blocks_bytes - at, &block) && block == two;
}

/* The golden slot read back record by record, as a mount walks one: each
 * kind in its place, each body exactly as written, PAD after the last. */
static TestResult TestSpineCircle(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     uuid[16];
    uint8_t    *s = g_test_output;
    uint32_t    at = 0, body = 0, len = 0, kinds = 0;
    uint64_t    tag = 0, flags = 0;
    SpineHead   head;
    SpineRibbon ribbon;
    SpineCounts counts;
    static const uint64_t order[7] = { SPINE_HEAD, SPINE_ROOT, SPINE_ROOT, SPINE_ROOT, SPINE_RIBBON,
                                       SPINE_COUNTS, SPINE_FLAGS };

    BookTestUuid(uuid);
    TallyTestGoldenSlotUnit(uuid);
    BookUnitStream(g_test_buffer, VOLUME_BLOCK_SECTORS, s);
    while (BookEntryNext(s, TALLY_SHEET_PAYLOAD, &at, &tag, &body, &len) == 1) {
        TEST_ASSERT(kinds < 7 && tag == order[kinds] << 1, "each record of its kind in its place");
        const uint8_t *b = s + body;
        if (kinds == 0)
            TEST_ASSERT(SpineHeadGet(b, len, &head) == TALLY_BODY_KNOWN && head.tally_seq == 5 &&
                        head.stitch_seq == 9 && head.stitch_clasp == 0xC0FFEE11u &&
                        head.made_ns == 1700000000123456789ull, "SpineHead as written");
        else if (kinds == 1)
            TEST_ASSERT(TallyTestRootIs(b, len, TALLY_FILES, 1, 70, 5, 1, 2048, 2304), "the FILES root");
        else if (kinds == 2)
            TEST_ASSERT(TallyTestRootIs(b, len, TALLY_TAGS, 0, 40, 5, 1, 2049, 2305), "the TAGS root");
        else if (kinds == 3)
            TEST_ASSERT(TallyTestRootIs(b, len, TALLY_DIGESTS, 1, 15000, 3, 2, 1500, 1756), "the DIGESTS root");
        else if (kinds == 4)
            TEST_ASSERT(SpineRibbonGet(b, len, &ribbon) == TALLY_BODY_KNOWN && ribbon.at == 2600 &&
                        ribbon.quire_block == 2560 && ribbon.quire_blocks == 256 && ribbon.next_block == 2816 &&
                        ribbon.next_blocks == 256, "SpineRibbon as written");
        else if (kinds == 5)
            TEST_ASSERT(SpineCountsGet(b, len, &counts) == TALLY_BODY_KNOWN && counts.next_fid == 71 &&
                        counts.next_tag == 52 && counts.next_bookmark == 1, "SpineCounts as written");
        else
            TEST_ASSERT(SpineFlagsGet(b, len, &flags) == TALLY_BODY_KNOWN && flags == SPINE_MEDIUM_LIED,
                        "SpineFlags: MEDIUM_LIED");
        if (kinds == 5)
            TEST_ASSERT_EQ(SpineCountsGet(b, len + 1u, &counts), TALLY_BODY_LONGER,
                           "SpineCounts a byte longer is a kernel that knows more");
        kinds++;
    }
    TEST_ASSERT_EQ(kinds, 7, "seven records, then PAD");
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
    { .name = "golden_node",    .func = TestGoldenNode,    .reach = TEST_READS_ONLY },
    { .name = "node_circle",    .func = TestTallyNodeCircle, .reach = TEST_READS_ONLY },
    { .name = "tally_elements", .func = TestTallyElements, .reach = TEST_READS_ONLY },
    { .name = "tally_refusals", .func = TestTallyRefusals, .reach = TEST_READS_ONLY },
    { .name = "digest_item_most", .func = TestDigestItemMost, .reach = TEST_READS_ONLY },
    { .name = "golden_slot",    .func = TestGoldenSlot,    .reach = TEST_READS_ONLY },
    { .name = "spine_circle",   .func = TestSpineCircle,   .reach = TEST_READS_ONLY },
};

const TestSuite g_book_suite = {
    .tests = g_book_tests, .count = ARRAY_SIZE(g_book_tests),
};
