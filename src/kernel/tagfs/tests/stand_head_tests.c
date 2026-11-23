#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../settle/binder.h"
#include "../settle/settle.h"
#include "boxos_decks.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_head suite: a file's first bytes, kept by its card
// (tagfs_constants.h TAGFS_FILE_HEAD, FileRecord.head)
//
// What a file is is told by its first bytes, by whoever knows what they mean:
// the volume keeps min(size, 16) of them in the file's record, exact through
// every change of what the file holds — a write of a Slip or of blocks, an
// append, a write over them, a cut and a growth — and through a mount off the
// ribbon and off a Tally; a change that leaves what the file holds as it was
// (its tail laid in its block) leaves them; and every live file of the
// volume, the image builder's among them, is held to what a read of it gives.
// The question of the files that begin with given bytes is answered off the
// records — the deck's storage.head.query too — and obj.getinfo says what
// the system keeps a file as and how it begins.
// ============================================================================

#define STAND_HEAD_MOST  TAGFS_HEAD_MOST
#define STAND_HEAD_PAGE  64u

static uint8_t s_head_block[TAGFS_BLOCK_SIZE];

/* Whether `fid`'s card keeps exactly `want`, `len` of them, the bit on
 * exactly when it keeps any. */
static bool StandHeadIs(uint64_t fid, const void* want, uint32_t len)
{
    TagFSMetadata md;
    if (tagfs_get_metadata(fid, &md) != 0)
        return false;
    bool same = md.head_len == len && ((md.flags & TAGFS_FILE_HEAD) != 0) == (len != 0) &&
                memcmp(md.head, want, len) == 0;
    tagfs_metadata_free(&md);
    return same;
}

/* Whether `fid`'s card keeps the bytes a read of the file begins with: 1, 0,
 * or -1 when the file would not be read. */
static int StandHeadReadIs(uint64_t fid)
{
    uint8_t          first[STAND_HEAD_MOST];
    uint64_t         got = 0;
    TagFSFileHandle* h   = tagfs_open(fid, TAGFS_HANDLE_READ);
    if (!h)
        return -1;
    error_t rc = tagfs_read(h, first, sizeof(first), &got);
    tagfs_close(h);
    return rc != OK ? -1 : StandHeadIs(fid, first, (uint32_t)got) ? 1 : 0;
}

static bool StandHeadRead(uint64_t fid)
{
    return StandHeadReadIs(fid) == 1;
}

static TestResult TestStandHeadWrites(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    TEST_ASSERT_EQ(tagfs_create_file("head-w.txt", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT(StandHeadIs(fid, NULL, 0), "a file of no bytes keeps none");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "hello", 5), 5, "five bytes are written");
    TEST_ASSERT(StandHeadIs(fid, "hello", 5), "it keeps the five");
    TEST_ASSERT_EQ(StandWriteAt(fid, 5, " world, and more after", 22), 22, "more go on at its end");
    TEST_ASSERT(StandHeadIs(fid, "hello world, and", 16), "it keeps its first sixteen");
    TEST_ASSERT_EQ(StandWriteAt(fid, 2, "LLO", 3), 3, "three are written over its third");
    TEST_ASSERT(StandHeadIs(fid, "heLLO world, and", 16), "and they are kept");
    TEST_ASSERT_EQ(StandWriteAt(fid, 20, "far", 3), 3, "three past them change nothing of them");
    TEST_ASSERT(StandHeadIs(fid, "heLLO world, and", 16) && StandHeadRead(fid), "they are what a read gives");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

static TestResult TestStandHeadBlocks(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    for (uint32_t i = 0; i < sizeof(s_head_block); i++)
        s_head_block[i] = (uint8_t)(i * 7u + 3u);
    TEST_ASSERT_EQ(tagfs_create_file("head-b.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, s_head_block, sizeof(s_head_block)), (int)sizeof(s_head_block),
                   "a whole block is written");
    TEST_ASSERT(StandHeadIs(fid, s_head_block, STAND_HEAD_MOST), "the card keeps its first bytes");
    TEST_ASSERT_EQ(StandWriteAt(fid, sizeof(s_head_block), s_head_block, sizeof(s_head_block)),
                   (int)sizeof(s_head_block), "a second block after it");
    TEST_ASSERT(StandHeadIs(fid, s_head_block, STAND_HEAD_MOST) && StandHeadRead(fid), "which changes none");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

static TestResult TestStandHeadSpans(void)
{
    static const uint8_t grown[STAND_HEAD_MOST] = { 'a', 'b', 'c', 'd', 'e' };
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    TEST_ASSERT_EQ(tagfs_create_file("head-s.txt", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "abcdefghijklmnopqrstuvwxyz", 26), 26, "26 bytes are written");
    TEST_ASSERT_OK(tagfs_span_file(fid, 5), "the file is cut to five");
    TEST_ASSERT(StandHeadIs(fid, "abcde", 5), "it keeps the five left");
    TEST_ASSERT_OK(tagfs_span_file(fid, 40), "and grown to forty");
    TEST_ASSERT(StandHeadIs(fid, grown, STAND_HEAD_MOST) && StandHeadRead(fid),
                "it keeps the five and the zeros the growth reads as");
    TEST_ASSERT_OK(tagfs_span_file(fid, 0), "cut to nothing");
    TEST_ASSERT(StandHeadIs(fid, NULL, 0), "it keeps none, its bit off");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

/* The tail laid in its block by the Tally a test asks: what the file holds is
 * as it was, and so are its first bytes. Then a mount off the ribbon and one
 * off a Tally read them as they were laid. */
static TestResult TestStandHeadLaidAndMounted(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           small = 0, other = 0;
    const char*        aside = StandBinderAside(&b);
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(tagfs_create_file("head-m.txt", NULL, 0, 0, &small), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(small, 0, "a note of mine", 14), 14, "its fourteen bytes ride a Slip");
    TEST_ASSERT_EQ(tagfs_create_file("head-n.txt", NULL, 0, 0, &other), 0, "and another");
    TEST_ASSERT_EQ(StandWriteAt(other, 0, "another, longer note", 20), 20, "with twenty");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle lays them");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh, the ribbon replayed");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14) && StandHeadIs(other, "another, longer ", 16),
                "the cards keep what the ribbon laid");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally is written, the tails laid in their blocks first");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14), "a tail laid in its block changes no byte of the file");
    TEST_ASSERT(StandStitchRemount(), "mounted again, off the Tally");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14) && StandHeadIs(other, "another, longer ", 16) &&
                StandHeadRead(small) && StandHeadRead(other), "the cards keep what the Tally laid");
out:
    (void)tagfs_delete_file(small, INK_WARRANTED);
    (void)tagfs_delete_file(other, INK_WARRANTED);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* Files the question tells apart: two that begin alike, one that begins
 * otherwise, one too short for the bytes asked, one kept deleted. */
typedef struct {
    uint64_t alike[2];
    uint64_t other;
    uint64_t shorter;
    uint64_t kept;
} StandHeads;

static const char s_head_mark[] = "\x01stand-head:";    /* a beginning no other file has */

static bool StandHeadsMake(StandHeads* h)
{
    memset(h, 0, sizeof(*h));
    return StandMakeFile("head-a.dat", "\x01stand-head:a, the first", 25, &h->alike[0]) &&
           StandMakeFile("head-c.dat", "\x01stand-head:b, the second", 26, &h->alike[1]) &&
           StandMakeFile("head-o.dat", "\x01stand-tail: the other", 23, &h->other) &&
           StandMakeFile("head-s.dat", "\x01stand-head", 11, &h->shorter) &&
           StandMakeFile("head-k.dat", "\x01stand-head:k, kept", 20, &h->kept) &&
           tagfs_keep_file(h->kept, 3600u, INK_WARRANTED) == 0;
}

static void StandHeadsClean(const StandHeads* h)
{
    const uint64_t all[] = { h->alike[0], h->alike[1], h->other, h->shorter, h->kept };
    for (uint32_t i = 0; i < ARRAY_SIZE(all); i++)
        (void)tagfs_delete_file(all[i], INK_WARRANTED);
}

static TestResult TestStandHeadStartingWith(void)
{
    TestResult verdict = TEST_PASS;
    StandHeads h;
    uint64_t   got[4];
    TagFSPage  page;
    TEST_ASSERT(StandHeadsMake(&h), "two files begin alike, one otherwise, one too short, one kept deleted");
    TEST_ASSERT_OK(TagFSStartingWithPage((const uint8_t*)s_head_mark, sizeof(s_head_mark) - 1u, 1, got, 4, true,
                                         &page), "the files that begin with the mark are asked");
    TEST_ASSERT(page.delivered == 2 && page.total == 2 && page.next == 0 && got[0] == h.alike[0] &&
                got[1] == h.alike[1], "the two, in id order: not the other, not the short one, not the kept one");
    TEST_ASSERT_OK(TagFSStartingWithPage((const uint8_t*)s_head_mark, sizeof(s_head_mark) - 1u, 1, got, 1, false,
                                         &page), "a page of one");
    TEST_ASSERT(page.delivered == 1 && got[0] == h.alike[0] && page.next == h.alike[1], "the first, the rest after");
    TEST_ASSERT_OK(TagFSStartingWithPage((const uint8_t*)"\x01stand-head:a, t", TAGFS_HEAD_MOST, 1, got, 4, false,
                                         &page), "all sixteen bytes a record keeps");
    TEST_ASSERT(page.delivered == 1 && got[0] == h.alike[0], "the one that begins with every one of them");
    TEST_ASSERT_OK(tagfs_restore_file(h.kept, INK_WARRANTED), "the kept file is brought back");
    TEST_ASSERT_OK(TagFSStartingWithPage((const uint8_t*)s_head_mark, sizeof(s_head_mark) - 1u, 1, got, 4, true,
                                         &page), "asked again");
    TEST_ASSERT(page.total == 3, "and it is met");
    TEST_ASSERT_EQ(TagFSStartingWithPage((const uint8_t*)s_head_mark, 0, 1, got, 4, false, &page),
                   ERR_INVALID_ARGUMENT, "no bytes is no question");
    TEST_ASSERT_EQ(TagFSStartingWithPage((const uint8_t*)"0123456789abcdefg", TAGFS_HEAD_MOST + 1u, 1, got, 4,
                                         false, &page),
                   ERR_INVALID_ARGUMENT, "nor more than a record keeps");
out:
    StandHeadsClean(&h);
    return verdict;
}

/* The deck's head question, and what obj.getinfo says of a file: asked as the
 * kernel asks it, its crates its own memory — with room for none and for one
 * too, through the one paging every list of the deck is answered by
 * (StorageAnswerPages): storage.query's and tag.shown's as well. */
static TestResult TestStandHeadDeck(void)
{
    static uint8_t answer[256];
    TestResult     verdict = TEST_PASS;
    StandHeads     h;
    uint64_t       said = 0, ids[2];
    uint32_t       head[2];
    TEST_ASSERT(StandHeadsMake(&h), "two files begin alike, one otherwise, one too short, one kept deleted");
    TEST_ASSERT_OK(StandDeckAnswer(STORAGE_HEAD_QUERY, NULL, 0, s_head_mark, sizeof(s_head_mark) - 1u, answer,
                                   sizeof(answer), &said, NULL), "storage.head.query of the mark");
    memcpy(head, answer, sizeof(head));
    memcpy(ids, answer + 8, sizeof(ids));
    TEST_ASSERT(said == 8u + 2u * sizeof(uint64_t) && head[0] == 2 && head[1] == 2 && ids[0] == h.alike[0] &&
                ids[1] == h.alike[1], "answers the two alike, in id order");
    TEST_ASSERT_OK(StandDeckAnswer(STORAGE_HEAD_QUERY, NULL, 0, s_head_mark, sizeof(s_head_mark) - 1u, answer, 8,
                                   &said, NULL), "asked with room for none");
    memcpy(head, answer, sizeof(head));
    TEST_ASSERT(said == 8 && head[0] == 0 && head[1] == 2, "it says how many there are: two");
    TEST_ASSERT_OK(StandDeckAnswer(STORAGE_HEAD_QUERY, NULL, 0, s_head_mark, sizeof(s_head_mark) - 1u, answer,
                                   8u + sizeof(uint64_t), &said, NULL), "asked with room for one");
    memcpy(head, answer, sizeof(head));
    memcpy(ids, answer + 8, sizeof(uint64_t));
    TEST_ASSERT(said == 16 && head[0] == 1 && head[1] == 2 && ids[0] == h.alike[0], "the first, of two");
    TEST_ASSERT_EQ(StandDeckAnswer(STORAGE_HEAD_QUERY, NULL, 0, "0123456789abcdefg", TAGFS_HEAD_MOST + 1u, answer,
                                   sizeof(answer), &said, NULL),
                   ERR_INVALID_ARGUMENT, "more bytes than a record keeps are refused");

    TEST_ASSERT_OK(StandKeepInRole(h.other, TAGFS_FILE_ROLE_SYSTEM), "the other is kept in the system's role");
    uint8_t params[9];
    memcpy(params, &h.other, 8);
    params[8] = STORAGE_TAGS_PERSON;
    TEST_ASSERT_OK(StandDeckAnswer(STORAGE_OBJ_GET_INFO, params, sizeof(params), NULL, 0, answer, sizeof(answer),
                                   &said, NULL), "its obj.getinfo");
    const uint8_t* tail = answer + said - (2u + TAGFS_HEAD_MOST);
    TEST_ASSERT(said > 2u + TAGFS_HEAD_MOST && tail[0] == 1 && tail[1] == TAGFS_HEAD_MOST &&
                memcmp(tail + 2, "\x01stand-tail: the", TAGFS_HEAD_MOST) == 0,
                "ends with its role, a system file, and its first sixteen bytes");
    memcpy(params, &h.shorter, 8);
    TEST_ASSERT_OK(StandDeckAnswer(STORAGE_OBJ_GET_INFO, params, sizeof(params), NULL, 0, answer, sizeof(answer),
                                   &said, NULL), "the short one's");
    tail = answer + said - (2u + 11u);
    TEST_ASSERT(tail[0] == 0 && tail[1] == 11 && memcmp(tail + 2, "\x01stand-head", 11) == 0,
                "an ordinary file of eleven bytes, all eleven of them");
out:
    StandHeadsClean(&h);
    return verdict;
}

/* Every live file of the volume — the image builder's, and every one the
 * tests before left — keeps what a read of it begins with. */
static TestResult TestStandHeadEveryFile(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   ids[STAND_HEAD_PAGE];
    uint64_t   from = 1;
    uint32_t   held = 0, wrong = 0, unread = 0;
    TagFSPage  page;
    do {
        TEST_ASSERT_OK(TagFSListPage(from, ids, STAND_HEAD_PAGE, false, &page), "a page of the live files");
        for (uint32_t i = 0; i < page.delivered; i++) {
            int is = StandHeadReadIs(ids[i]);
            held  += is == 1;
            unread += is < 0;
            if (is == 0) {
                wrong++;
                kprintf("[TESTS] stand_head: file %llu keeps first bytes a read of it does not begin with\n",
                        (unsigned long long)ids[i]);
            }
        }
        from = page.next;
    } while (from);
    TestEvidence("live files held to what a read of them begins with:", held);
    TestEvidence("live files no read would give, held to nothing here:", unread);
    TEST_ASSERT(held > 0 && wrong == 0, "every one read keeps them");
out:
    return verdict;
}

static const TestCase g_stand_head_tests[] = {
    { .name = "stand_head_writes", .func = TestStandHeadWrites, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_blocks", .func = TestStandHeadBlocks, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_spans", .func = TestStandHeadSpans, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_laid_and_mounted", .func = TestStandHeadLaidAndMounted, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_starting_with", .func = TestStandHeadStartingWith, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_deck", .func = TestStandHeadDeck, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_every_file", .func = TestStandHeadEveryFile, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_head_suite = {
    g_stand_head_tests, sizeof(g_stand_head_tests) / sizeof(g_stand_head_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
