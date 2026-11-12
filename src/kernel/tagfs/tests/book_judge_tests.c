#include "tests.h"
#include "volume_spine.h"

// ============================================================================
// The decisions every reader of a book makes alike, and makes out of the same
// headers (volume_tally.h, volume_spine.h): the element of a DIGESTS leaf that
// names a block, a slot's shape judged record by record with a reason, and
// the slot a book is read from — each held on the golden slot (book_tests.c)
// or on a leaf the codec lays, what it finds, what it refuses and why. The
// kernel's mount reads by them and the UEFI loader is to pick its kernel by
// them (spec_v2 §16.7 п.2): one that drifted would stand the two on two
// slots, or hold a block to another element's digest.
//
// Arithmetic over memory and nothing else, like the book suite beside it: no
// medium is touched, so the suite is in every build, on every boot that
// mounts a volume (tests.c).
// ============================================================================

/* The digests of the leaf JudgeDigestLeaf lays, in its blocks' order: 1024 to
 * 1026, 1100 and 1101, 1500 — the first the golden block's (volume_digest.h). */
static const uint64_t g_judge_digests[6] = { 0x0911A376FAC2E968ull, 1, 0xFFFFFFFFFFFFFFFFull,
                                             0x8000000000000000ull, 42, 0x0102030405060708ull };

/* A DIGESTS leaf of three elements, laid by the codec into `s` with PAD to the
 * end of a Sheet's room: its NodeHead counts `items` of them, and when `cut`
 * the second element's body says three digests and carries two. */
static void JudgeDigestLeaf(uint8_t *s, uint64_t items, int cut)
{
    static const TallyKey keys[3]  = { { 1024, 0 }, { 1100, 0 }, { 1500, 0 } };
    static const uint32_t count[3] = { 3, 2, 1 }, from[3] = { 0, 3, 5 };
    NodeHead h = { .tree = TALLY_DIGESTS, .level = 0, .items = items, .first = keys[0] };
    uint8_t  body[32];

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    uint32_t at = NodeHeadPut(s, &h);
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t len = TallyDigestsPut(body, g_judge_digests + from[i], count[i]);
        if (cut && i == 1)
            body[0] = 3;
        at += TallyItemPut(s + at, TALLY_DIGESTS, keys[i], keys[i ? i - 1u : 0], body, len);
    }
}

/* What TallyDigestIn answers for `block` on the leaf at `s`, the digest it
 * gives in *digest — left at 0 when it gives none. */
static int JudgeDigestOf(const uint8_t *s, uint64_t block, uint64_t *digest)
{
    *digest = 0;
    return TallyDigestIn(s, TALLY_SHEET_PAYLOAD, block, digest);
}

/* The element of a DIGESTS leaf that names a block: each block of each
 * element found with its own digest; a block before the first, between two,
 * past the last named by none; an element that does not read refuses every
 * block it or a later one would name, and none before it — the walk ends at
 * the first element past the block; a leaf that counts an element it does not
 * carry, and a stream that opens with no NodeHead, refused. */
static TestResult TestDigestInLeaf(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t       *s = g_test_output;
    uint64_t       d = 0;
    static const uint64_t named[6] = { 1024, 1025, 1026, 1100, 1101, 1500 };
    static const uint64_t none[7]  = { 0, 1023, 1027, 1099, 1102, 1501, 0xFFFFFFFFFFFFFFFFull };

    JudgeDigestLeaf(s, 3, 0);
    for (uint32_t i = 0; i < 6; i++) {
        TEST_ASSERT_EQ(JudgeDigestOf(s, named[i], &d), 1, "a block an element names is found");
        TEST_ASSERT(d == g_judge_digests[i], "with the digest the element carries for it, little-endian, whole");
    }
    for (uint32_t i = 0; i < 7; i++)
        TEST_ASSERT_EQ(JudgeDigestOf(s, none[i], &d), 0, "a block before, between or past the elements is named by none");

    JudgeDigestLeaf(s, 3, 1);
    TEST_ASSERT(JudgeDigestOf(s, 1025, &d) == 1 && d == g_judge_digests[1],
                "an element before one that does not read is found");
    TEST_ASSERT_EQ(JudgeDigestOf(s, 1050, &d), 0, "a block the walk passes no broken element for is named by none");
    TEST_ASSERT_EQ(JudgeDigestOf(s, 1100, &d), -1, "a block the broken element would name is refused");
    TEST_ASSERT_EQ(JudgeDigestOf(s, 1500, &d), -1, "and so is one past it: the walk reads it on the way");

    JudgeDigestLeaf(s, 4, 0);
    TEST_ASSERT(JudgeDigestOf(s, 1500, &d) == 1 && d == g_judge_digests[5], "a leaf counting four still names 1500");
    TEST_ASSERT_EQ(JudgeDigestOf(s, 1501, &d), -1, "but past it the fourth element it counts does not read");

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    TEST_ASSERT_EQ(JudgeDigestOf(s, 1024, &d), -1, "a stream that opens with no NodeHead is refused");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// A slot's shape
// ----------------------------------------------------------------------------

/* The answer a walk of the stream at `s` ends on — WHOLE, or the first torn
 * one — and the walk in *w; how many UNKNOWN and LONGER it met in *glass. */
static SpineSays JudgeWalk(const uint8_t *s, uint32_t room, SpineWalk *w, uint32_t *glass)
{
    SpineSays says;
    memset(w, 0, sizeof(*w));
    *glass = 0;
    do {
        says = SpineSlotStep(s, room, w);
        *glass += says == SPINE_SAYS_UNKNOWN || says == SPINE_SAYS_LONGER;
    } while (says != SPINE_SAYS_WHOLE && !SpineSaysTorn(says));
    return says;
}

/* The golden slot's stream into `s`, PAD to the end of its room, and `n` bytes
 * of `more` laid past its last record: the bytes the stream takes. */
static uint32_t JudgeGoldenAnd(uint8_t *s, const uint8_t *more, uint32_t n)
{
    uint32_t starts[7];
    memset(s, 0, TALLY_SHEET_PAYLOAD);
    uint32_t len = TallyTestGoldenSlot(s, starts);
    memcpy(s + len, more, n);
    return len + n;
}

/* A root of `tree` as the golden slot's FILES root is (seq 5, one Sheet, 2048
 * and 2304), its body `extra` bytes of zeros longer — `cut` bytes shorter —
 * than this reader knows: the bytes the record takes. */
static uint32_t JudgeRoot(uint8_t *out, uint64_t tree, uint32_t extra, uint32_t cut)
{
    static const uint64_t blocks[2] = { 2048, 2304 };
    uint8_t  body[64];
    uint32_t len = BookVarintPut(body, tree);
    len += BookVarintPut(body + len, 0);
    len += BookVarintPut(body + len, 1);
    len += BookVarintPut(body + len, 70);
    len += BookPtrPut(body + len, 5, 1, blocks, 2);
    for (uint32_t i = 0; i < extra; i++)
        body[len++] = 0;
    len -= cut;
    uint32_t at = TallyRecordOpen(out, SPINE_ROOT, len);
    memcpy(out + at, body, len);
    return at + len;
}

/* The golden slot walked: every record RECORD where the codec laid it, of its
 * kind, the roots of FILES, TAGS and DIGESTS in turn, and WHOLE at PAD — what
 * its records say read into the walk. */
static TestResult TestSlotShapeGolden(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t   *s = g_test_output;
    uint32_t   starts[7], roots = 0;
    SpineWalk  w;
    static const uint64_t kinds[7] = { SPINE_HEAD, SPINE_ROOT, SPINE_ROOT, SPINE_ROOT, SPINE_RIBBON,
                                       SPINE_COUNTS, SPINE_FLAGS };
    static const uint64_t trees[3] = { TALLY_FILES, TALLY_TAGS, TALLY_DIGESTS };

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    TallyTestGoldenSlot(s, starts);
    memset(&w, 0, sizeof(w));
    for (uint32_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQ(w.at, starts[i], "each step stands where the codec laid the record");
        TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_RECORD, "each record reads");
        TEST_ASSERT_EQ(w.kind, kinds[i], "of its kind, in its place");
        if (w.kind == SPINE_ROOT)
            TEST_ASSERT_EQ(w.root.tree, trees[roots++], "the roots in the order the slot names them");
    }
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_WHOLE, "PAD ends a slot that holds");
    TEST_ASSERT_EQ(w.nth, 7, "seven records walked");
    TEST_ASSERT(w.head.tally_seq == 5 && w.head.stitch_seq == 9 && w.head.stitch_clasp == 0xC0FFEE11u &&
                w.head.made_ns == 1700000000123456789ull, "SpineHead read into the walk");
    TEST_ASSERT(w.ribbon.at == 2600 && w.ribbon.quire_block == 2560 && w.ribbon.next_block == 2816,
                "SpineRibbon read into the walk");
    TEST_ASSERT(w.counts.next_fid == 71 && w.counts.next_tag == 52 && w.counts.next_bookmark == 1,
                "SpineCounts read into the walk");
    TEST_ASSERT_EQ(w.flags, SPINE_MEDIUM_LIED, "SpineFlags read into the walk");
out:
    return verdict;
}

/* A slot torn for each rule of its shape, by the first it breaks, with the
 * reason and what it is about: the first record not SpineHead — not even
 * one a reader may step over; a second SpineHead and a second SpineRibbon; a
 * root whose body does not read, before a tree named twice is asked; a tree
 * named twice, before a body longer than known is; a record running past the
 * slot; no SpineRibbon, no SpineCounts, an empty stream. */
static TestResult TestSlotShapeTorn(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t    *s = g_test_output, more[64];
    uint32_t    glass = 0, n, len;
    SpineWalk   w;
    SpineHead   head   = { .tally_seq = 5, .stitch_seq = 9 };
    SpineRibbon ribbon = { .at = 2600 };
    SpineCounts counts = { .next_fid = 71 };

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    n = SpineCountsPut(s, &counts);
    SpineHeadPut(s + n, &head);
    TEST_ASSERT_EQ(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass), SPINE_SAYS_NOT_HEAD, "SpineCounts first: torn");
    TEST_ASSERT(w.kind == SPINE_COUNTS && w.nth == 1, "for its first record, of kind SpineCounts");
    memset(s, 0, TALLY_SHEET_PAYLOAD);
    n = TallyRecordOpen(s, 9, 0);
    s[0] |= 1u;
    SpineHeadPut(s + n, &head);
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_NOT_HEAD && w.kind == 9,
                "a record a reader may step over is no SpineHead either");

    JudgeGoldenAnd(s, more, SpineHeadPut(more, &head));
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_SECOND && w.kind == SPINE_HEAD,
                "a second SpineHead: torn");
    JudgeGoldenAnd(s, more, SpineRibbonPut(more, &ribbon));
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_SECOND && w.kind == SPINE_RIBBON,
                "a second SpineRibbon: torn");
    JudgeGoldenAnd(s, more, JudgeRoot(more, TALLY_TAGS, 0, 3));
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_BROKEN && w.kind == SPINE_ROOT,
                "a root of TAGS again, cut short: its body does not read, before its tree is asked");
    JudgeGoldenAnd(s, more, JudgeRoot(more, TALLY_TAGS, 1, 0));
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_TREE_TWICE &&
                w.root.tree == TALLY_TAGS && glass == 0, "a root of TAGS again, a byte longer: TAGS named twice");

    more[0] = SPINE_FLAGS << 1;
    more[1] = 100;
    len = JudgeGoldenAnd(s, more, 2);
    TEST_ASSERT(JudgeWalk(s, len + 10u, &w, &glass) == SPINE_SAYS_RUNS_PAST && w.at == len - 2u,
                "a record whose length runs past the slot: torn, at the byte it begins");

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    n = SpineHeadPut(s, &head);
    SpineCountsPut(s + n, &counts);
    TEST_ASSERT_EQ(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass), SPINE_SAYS_NO_RIBBON, "no SpineRibbon: torn");
    memset(s, 0, TALLY_SHEET_PAYLOAD);
    n = SpineHeadPut(s, &head);
    SpineRibbonPut(s + n, &ribbon);
    TEST_ASSERT_EQ(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass), SPINE_SAYS_NO_COUNTS, "no SpineCounts: torn");
    memset(s, 0, TALLY_SHEET_PAYLOAD);
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_NO_RIBBON && w.nth == 0,
                "a stream of PAD alone carries none of them");
out:
    return verdict;
}

/* What a slot carries that only a kernel knowing more could write — a kind
 * not to be stepped over, a body longer than known, of a root among them —
 * said and the walk going on to WHOLE; a kind that may be stepped over, and
 * SpineFlags again, the last one standing, read as any record. */
static TestResult TestSlotShapeGlass(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t    *s = g_test_output, more[64];
    uint32_t    glass = 0, n;
    SpineWalk   w;
    SpineHead   head   = { .tally_seq = 5, .stitch_seq = 9 };
    SpineRibbon ribbon = { .at = 2600 };
    SpineCounts counts = { .next_fid = 71, .next_tag = 52, .next_bookmark = 1 };

    n = TallyRecordOpen(more, 9, 2);
    more[n] = more[n + 1u] = 7;
    JudgeGoldenAnd(s, more, n + 2u);
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_WHOLE && glass == 1 && w.nth == 8,
                "a kind not to be stepped over: said once, and the slot holds");
    more[0] |= 1u;
    JudgeGoldenAnd(s, more, n + 2u);
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_WHOLE && glass == 0 && w.nth == 8,
                "a kind that may be stepped over: nothing said");
    JudgeGoldenAnd(s, more, SpineFlagsPut(more, 0));
    TEST_ASSERT(JudgeWalk(s, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_WHOLE && glass == 0 && w.flags == 0,
                "SpineFlags again: the last one stands");

    memset(s, 0, TALLY_SHEET_PAYLOAD);
    n = SpineHeadPut(s, &head);
    n += JudgeRoot(s + n, TALLY_FILES, 2, 0);
    n += SpineRibbonPut(s + n, &ribbon);
    uint32_t c = TallyRecordOpen(s + n, SPINE_COUNTS, 4);
    BookVarintPut(s + n + c, 71);
    BookVarintPut(s + n + c + 1u, 52);
    BookVarintPut(s + n + c + 2u, 1);
    memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_RECORD, "SpineHead reads");
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_LONGER, "a root two bytes longer is said");
    TEST_ASSERT(w.longer == 2 && w.root.tree == TALLY_FILES && w.root.items == 70 && w.root.ptr.seq == 5,
                "by the two bytes past its pointer, the root read all the same");
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_RECORD, "SpineRibbon reads");
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_LONGER, "SpineCounts a byte longer is said");
    TEST_ASSERT(w.longer == 1 && w.counts.next_fid == counts.next_fid && w.counts.next_bookmark == 1,
                "by the byte, its counts read all the same");
    TEST_ASSERT_EQ(SpineSlotStep(s, TALLY_SHEET_PAYLOAD, &w), SPINE_SAYS_WHOLE, "and the slot holds");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// The slot stood on
// ----------------------------------------------------------------------------

/* Ring slot `i` of JudgeRing's: the golden slot's stream sealed at `seq` at
 * block 256 + 256 * i, into g_test_buffer's i-th block. */
static void JudgeRingSlot(uint32_t i, uint64_t seq, const uint8_t uuid[16])
{
    uint32_t starts[7];
    memset(g_test_output, 0, TALLY_SHEET_PAYLOAD);
    uint32_t len = TallyTestGoldenSlot(g_test_output, starts);
    TallyUnitSeal(g_test_buffer + (uint64_t)i * VOLUME_BLOCK_BYTES, g_test_output, len, starts, 7, BOOK_UNIT_SLOT,
                  seq, (256ull + 256ull * i) * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, uuid);
}

/* Whether ring slot `i` holds — one SLOT unit where it lies, its stream WHOLE
 * — its seq in *seq. */
static int JudgeRingHolds(uint32_t i, const uint8_t uuid[16], uint64_t *seq)
{
    const uint8_t *sectors = g_test_buffer + (uint64_t)i * VOLUME_BLOCK_BYTES;
    BookSectorHead head;
    SpineWalk      w;
    uint32_t       glass;
    if (!BookUnitCheck(sectors, VOLUME_BLOCK_SECTORS, uuid, (256ull + 256ull * i) * VOLUME_BLOCK_SECTORS, &head) ||
        head.kind != BOOK_UNIT_SLOT)
        return 0;
    BookUnitStream(sectors, VOLUME_BLOCK_SECTORS, g_test_output);
    *seq = head.seq;
    return JudgeWalk(g_test_output, TALLY_SHEET_PAYLOAD, &w, &glass) == SPINE_SAYS_WHOLE;
}

/*
 * The order slots are read in, and which roots must read: the newer first,
 * of one seq the lower index; FILES to DIGESTS authoritative unless DERIVED,
 * MAP, SHARED and a tree no reader knows not. And a ring of three golden
 * slots — slot 0 the newest, seq 9, with a sector spoiled; slot 1 seq 5; slot
 * 2 seq 7 — read as every reader of a book reads it: slot 2, then slot 1; its
 * three authoritative roots each to read; and slot 1 stood on when slot 2's
 * do not.
 */
static TestResult TestSlotChosen(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    uuid[16];
    uint64_t   seq[3] = { 0, 0, 0 };
    uint32_t   order[3], held = 0, named = 0;
    SpineWalk  w;
    SpineSays  says;
    SpineRoot  r = { .tree = TALLY_FILES };

    TEST_ASSERT(SpineSlotNewer(5, 2, 2, 0) && !SpineSlotNewer(2, 0, 5, 2), "the larger seq is read first");
    TEST_ASSERT(SpineSlotNewer(3, 0, 3, 1) && !SpineSlotNewer(3, 1, 3, 0), "of one seq, the lower index");
    TEST_ASSERT(!SpineSlotNewer(3, 1, 3, 1), "a slot is not read before itself");
    for (r.tree = TALLY_FILES; r.tree <= TALLY_DIGESTS; r.tree++)
        TEST_ASSERT(SpineTreeAuthoritative(&r), "FILES, TAGS, USE, BOOKMARKS, LIEN and DIGESTS must read");
    for (r.tree = TALLY_MAP; r.tree <= TALLY_SHARED + 1u; r.tree++)
        TEST_ASSERT(!SpineTreeAuthoritative(&r), "MAP, SHARED and a tree no reader knows need not");
    r.tree  = TALLY_FILES;
    r.flags = TALLY_ROOT_DERIVED;
    TEST_ASSERT(!SpineTreeAuthoritative(&r), "nor a root that says it is derived");
    r.tree  = 0;
    r.flags = 0;
    TEST_ASSERT(!SpineTreeAuthoritative(&r), "nor tree 0");

    BookTestUuid(uuid);
    JudgeRingSlot(0, 9, uuid);
    JudgeRingSlot(1, 5, uuid);
    JudgeRingSlot(2, 7, uuid);
    g_test_buffer[3 * BOOK_SECTOR_BYTES + 100] ^= 0x10u;
    for (uint32_t i = 0; i < 3; i++) {
        if (!JudgeRingHolds(i, uuid, &seq[i]))
            continue;
        uint32_t at = 0;
        while (at < held && !SpineSlotNewer(seq[i], i, seq[order[at]], order[at]))
            at++;
        for (uint32_t k = held; k > at; k--)
            order[k] = order[k - 1u];
        order[at] = i;
        held++;
    }
    TEST_ASSERT_EQ(held, 2, "slot 0, a sector of it spoiled, does not hold: two do");
    TEST_ASSERT_EQ(order[0], 2, "slot 2, of seq 7, is read first");
    TEST_ASSERT_EQ(order[1], 1, "slot 1, of seq 5, next");

    memset(&w, 0, sizeof(w));
    do {
        says = SpineSlotStep(g_test_output, TALLY_SHEET_PAYLOAD, &w);
        named += says == SPINE_SAYS_RECORD && w.kind == SPINE_ROOT && SpineTreeAuthoritative(&w.root);
    } while (says == SPINE_SAYS_RECORD);
    TEST_ASSERT_EQ(named, 3, "its roots of FILES, TAGS and DIGESTS are the ones that must read");
    bool roots_read[3] = { true, true, false };
    uint32_t stood = 3;
    for (uint32_t k = 0; k < held && stood == 3; k++)
        if (roots_read[order[k]])
            stood = order[k];
    TEST_ASSERT_EQ(stood, 1, "slot 2's roots not reading, the book stands on slot 1");
out:
    return verdict;
}

static const TestCase g_book_judge_tests[] = {
    { .name = "digest_in_leaf",    .func = TestDigestInLeaf,    .reach = TEST_READS_ONLY },
    { .name = "slot_shape_golden", .func = TestSlotShapeGolden, .reach = TEST_READS_ONLY },
    { .name = "slot_shape_torn",   .func = TestSlotShapeTorn,   .reach = TEST_READS_ONLY },
    { .name = "slot_shape_glass",  .func = TestSlotShapeGlass,  .reach = TEST_READS_ONLY },
    { .name = "slot_chosen",       .func = TestSlotChosen,      .reach = TEST_READS_ONLY },
};

const TestSuite g_book_judge_suite = {
    .tests = g_book_judge_tests, .count = ARRAY_SIZE(g_book_judge_tests),
};
