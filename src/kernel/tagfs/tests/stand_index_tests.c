#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "op_registry.h"
#include "boxos_decks.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The indexes of the live files (step 1.3b)
//
// What memory knows about the files — the pool's newest record of each, the
// file table's rows, who wears every tag — used to be arrays as long as the
// highest file id the volume had ever issued, and every question of which
// files there are was answered into an array cut at a ceiling. These tests
// ask a map and an index of their own what a spread of a million ids costs
// and how a tag's members cross the edge of a window, and ask the volume for
// more files than the old ceiling of 1024 held, a page at a time. And the
// other end of the ids (step 1.4): the last one a volume can issue.
// ============================================================================

#define STAND_INDEX_FILES     64u
#define STAND_INDEX_SPREAD    (1u << 20)
#define STAND_INDEX_PER_FILE  256u     /* what the map may cost a live file, its record included */
#define STAND_INDEX_PER_MEMBER 64u     /* what a member of a spread tag may cost the index */
#define STAND_INDEX_MANY      1100u    /* more files than TAGFS_MAX_FILES (1024) held */
#define STAND_INDEX_PAGE      100u
#define STAND_INDEX_CRATE     1200u    /* ids the deck's crate has room for */
#define STAND_INDEX_TAGS      1500u
#define STAND_INDEX_KEY_TAGS  2000u    /* the values of one key the query test asks */
#define STAND_INDEX_KEY_FIRST 10u      /* the first tag id of them */

static char     s_index_name[] = "stand_index";
static uint64_t s_rows[STAND_INDEX_FILES + 1];
static uint64_t s_made[STAND_INDEX_MANY];
static uint64_t s_page[STAND_INDEX_PAGE];
static uint64_t s_crate[1 + STAND_INDEX_CRATE];   /* [u32 delivered][u32 total], the ids */
static uint8_t  s_query_op[sizeof(ManifestOp) + 8] __attribute__((aligned(8)));
static Crate    s_query_crates[2];

/* STAND_INDEX_FILES files into a map of the test's own, their ids `step`
 * apart from 1 on, each with a record of one tag and a row. */
static bool StandIndexFill(FileMap* map, uint32_t step)
{
    uint32_t tag = 1;
    for (uint32_t k = 0; k < STAND_INDEX_FILES; k++) {
        uint64_t      fid  = 1 + k * step;
        TagFSMetadata meta = { .file_id = fid, .flags = TAGFS_FILE_ACTIVE,
                               .filename = s_index_name, .tag_ids = &tag, .tag_count = 1 };
        FileRecord*   rec   = FileRecordBuild(&meta);
        FileMapStake  stake = { .count = 0 };
        if (!rec || FileMapStakeFor(map, fid, &stake) != OK) {
            FileRecordRelease(rec);
            return false;
        }
        RecordWhere   where = { .link = 0, .offset = (uint16_t)(16 + k), .gen = 0, .shape = 0 };
        RecordWhere   was;
        error_t       put = FileMapRecordPut(map, fid, rec, &where, 0, &stake, &was);
        FileMapStakeLetGo(&stake);
        if (put != OK || FileMapRowSet(map, fid, 3, 16 + k) != OK)
            return false;
    }
    return true;
}

static TestResult TestStandIndexMapCostsTheLive(void)
{
    TestResult      verdict = TEST_PASS;
    FileMap*        near    = FileMapCreate();
    FileMap*        far     = FileMapCreate();
    TagBitmapIndex* idx     = tag_bitmap_create();
    uint32_t        step    = (STAND_INDEX_SPREAD - 1) / (STAND_INDEX_FILES - 1);

    TEST_ASSERT(near && far && idx, "two maps and an index of the test's own");
    TEST_ASSERT(StandIndexFill(near, 1), "64 files side by side, ids 1 to 64, are held");
    TEST_ASSERT(StandIndexFill(far, step), "64 files spread up to id 2^20 are held");
    TEST_ASSERT(1 + (STAND_INDEX_FILES - 1) * step > STAND_INDEX_SPREAD - step,
                "the last of them lies within one step of 2^20");
    TEST_ASSERT_EQ(FileMapCount(far), STAND_INDEX_FILES, "the map counts its 64 files");

    size_t near_bytes = FileMapBytes(near);
    size_t far_bytes  = FileMapBytes(far);
    TEST_ASSERT_EQ(far_bytes, near_bytes, "ids a million apart cost what ids side by side do");
    TEST_ASSERT(far_bytes <= (size_t)STAND_INDEX_FILES * STAND_INDEX_PER_FILE,
                "and no more than 256 bytes a live file, where a dense mirror paid for every id");

    TEST_ASSERT_EQ(FileMapRows(far, 1, s_rows, ARRAY_SIZE(s_rows)), STAND_INDEX_FILES,
                   "the rows are the 64 live files and nothing for the ids between them");
    for (uint32_t k = 0; k < STAND_INDEX_FILES; k++)
        TEST_ASSERT_EQ(s_rows[k], 1 + k * step, "in id order");

    for (uint32_t k = 0; k < STAND_INDEX_FILES; k += 2)
        FileMapDrop(far, 1 + k * step, NULL);
    TEST_ASSERT_EQ(FileMapCount(far), STAND_INDEX_FILES / 2, "half of them dropped");
    TEST_ASSERT(FileMapBytes(far) < far_bytes, "and the memory falls with them");

    for (uint32_t k = 0; k < STAND_INDEX_FILES; k++)
        TEST_ASSERT_EQ(tag_bitmap_set(idx, 2, 1 + k * step), 0, "a tag worn by the spread files");
    TEST_ASSERT(TagBitmapBytes(idx) <= (uint64_t)STAND_INDEX_FILES * STAND_INDEX_PER_MEMBER,
                "costs its members, not a bitmap as wide as the highest of them");
out:
    FileMapDestroy(near);
    FileMapDestroy(far);
    tag_bitmap_destroy(idx);
    return verdict;
}

// ----------------------------------------------------------------------------
// A tag's members across the edge of a window of 65536 ids
// ----------------------------------------------------------------------------

static const uint32_t s_edge[] = { 1, 65535, 65536, 65537, 131071, 131072 };

/* The members of `tags` (all of them worn) from `from` on, `room` at most. */
static bool StandIndexPage(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                           uint32_t from, uint32_t room, TagFSPage* page)
{
    return TagBitmapQueryPage(idx, tags, count, NULL, 0, NULL, 0, from, s_page, room,
                              true, page) == 0;
}

/* `fid` out from under `tag` the way an Ink takes it out: under the volume's
 * state lock, which the index changes shape under alone. */
static void StandIndexLeave(TagBitmapIndex* idx, uint32_t tag, uint64_t fid)
{
    TagFSLockTake(&tagfs_get_state()->lock);
    TagBitmapLeave(idx, tag, fid);
    TagFSLockDrop(&tagfs_get_state()->lock);
}

static TestResult TestStandIndexWindowEdge(void)
{
    TestResult      verdict = TEST_PASS;
    TagBitmapIndex* idx     = tag_bitmap_create();
    uint32_t        tag     = 3;
    uint32_t        both[2] = { 3, 4 };
    TagFSPage       page;

    TEST_ASSERT(idx != NULL, "an index of the test's own");
    for (uint32_t i = 0; i < ARRAY_SIZE(s_edge); i++)
        TEST_ASSERT_EQ(tag_bitmap_set(idx, tag, s_edge[i]), 0, "members on both sides of 65536");
    for (uint32_t i = 0; i < ARRAY_SIZE(s_edge); i++)
        TEST_ASSERT(TagBitmapHas(idx, tag, s_edge[i]), "every member is the tag's");
    TEST_ASSERT(!TagBitmapHas(idx, tag, 65534) && !TagBitmapHas(idx, tag, 65538) &&
                !TagBitmapHas(idx, tag, 131070), "and the ids beside them are not");

    TEST_ASSERT(StandIndexPage(idx, &tag, 1, 65535, 2, &page), "a page from 65535 on");
    TEST_ASSERT_EQ(page.delivered, 2, "two ids in a room of two");
    TEST_ASSERT(s_page[0] == 65535 && s_page[1] == 65536, "the last of one window, the first of the next");
    TEST_ASSERT_EQ(page.next, 65537, "and the rest begin at 65537");
    TEST_ASSERT_EQ(page.total, 5, "five members from 65535 on");

    TEST_ASSERT(tag_bitmap_set(idx, 4, 65535) == 0 && tag_bitmap_set(idx, 4, 131071) == 0 &&
                tag_bitmap_set(idx, 4, 131072) == 0, "a second tag on three of them");
    TEST_ASSERT(StandIndexPage(idx, both, 2, 1, 8, &page), "the files wearing both");
    TEST_ASSERT(page.delivered == 3 && s_page[0] == 65535 && s_page[1] == 131071 &&
                s_page[2] == 131072, "are the three, found across two edges");

    /* The window past the edge filled until an array would outweigh its
     * bitmap, then emptied until the array is the lighter again. */
    for (uint32_t i = 1; i <= 4096; i++)
        TEST_ASSERT_EQ(tag_bitmap_set(idx, tag, 65536 + 2 * i), 0, "4096 more members past the edge");
    TEST_ASSERT(TagBitmapBytes(idx) >= 8192, "the window became a bitmap of 8 KiB");
    TEST_ASSERT(StandIndexPage(idx, &tag, 1, 65535, 0, &page), "counted from 65535 on");
    TEST_ASSERT_EQ(page.total, 4101, "every member of the bitmap and both windows beside it");
    TEST_ASSERT(TagBitmapHas(idx, tag, 65536 + 2 * 4096) && !TagBitmapHas(idx, tag, 65539),
                "the bitmap holds the members and only them");
    for (uint32_t i = 1; i <= 4096; i++)
        StandIndexLeave(idx, tag, 65536 + 2 * i);
    TEST_ASSERT(TagBitmapBytes(idx) >= 8192, "the leaves allocated nothing: still a bitmap");
    TEST_ASSERT(!TagBitmapHas(idx, tag, 65536 + 2 * 4096), "but the members left it");
    TagBitmapShrink(idx, &tagfs_get_state()->lock, &tag, 1, 65536 + 2 * 4096);
    TEST_ASSERT(TagBitmapBytes(idx) < 8192, "the window is an array again after the last leave");
    TEST_ASSERT(StandIndexPage(idx, &tag, 1, 1, 8, &page), "a page of everything");
    TEST_ASSERT_EQ(page.delivered, ARRAY_SIZE(s_edge), "the six members are back to six");
    for (uint32_t i = 0; i < ARRAY_SIZE(s_edge); i++)
        TEST_ASSERT_EQ(s_page[i], s_edge[i], "each where it was, in order");

    StandIndexLeave(idx, tag, 65536);
    TEST_ASSERT(TagBitmapHas(idx, tag, 65535) && !TagBitmapHas(idx, tag, 65536) &&
                TagBitmapHas(idx, tag, 65537), "and nothing on either side of it moved");
out:
    tag_bitmap_destroy(idx);
    return verdict;
}

// ----------------------------------------------------------------------------
// A wildcard weighed by its tags under the index's lock
// ----------------------------------------------------------------------------

/*
 * A key with 2000 values, each worn by a file of its own, asked whole: every
 * leap of the query asks all 2000 tags, and choosing the lead sizes them all.
 * The index's lock keeps interrupts off, and a query's leap used to be one step
 * of a hold's 512 however many tags it asked — a hold of a million asks. Now
 * no hold gives the query more than its steps and the one ask that crosses
 * them, however many values the key has.
 */
static TestResult TestStandIndexQueryHoldBounded(void)
{
    TestResult      verdict = TEST_PASS;
    TagBitmapIndex* idx     = tag_bitmap_create();
    uint32_t*       ids     = kmalloc(sizeof(uint32_t) * STAND_INDEX_KEY_TAGS);
    TagFSPage       page;

    TEST_ASSERT(idx && ids, "an index and a key of the test's own");
    for (uint32_t i = 0; i < STAND_INDEX_KEY_TAGS; i++) {
        ids[i] = (uint32_t)(STAND_INDEX_KEY_FIRST + i);
        TEST_ASSERT_EQ(tag_bitmap_set(idx, ids[i], 1 + 3 * i), 0,
                       "each value of the key worn by a file of its own");
    }

    TagIdList key = { .ids = ids, .count = STAND_INDEX_KEY_TAGS, .capacity = 0 };
    (void)TagBitmapQueryMost(idx);
    TEST_ASSERT_EQ(TagBitmapQueryPage(idx, NULL, 0, &key, 1, NULL, 0, 1, s_page,
                                      STAND_INDEX_PAGE, true, &page), 0, "the key asked whole");
    uint32_t most = TagBitmapQueryMost(idx);
    TEST_ASSERT_EQ(page.total, STAND_INDEX_KEY_TAGS, "every file wearing a value is counted");
    TEST_ASSERT_EQ(page.delivered, STAND_INDEX_PAGE, "a page of them delivered");
    for (uint32_t k = 0; k < STAND_INDEX_PAGE; k++)
        TEST_ASSERT_EQ(s_page[k], 1 + 3 * k, "in id order, none missed");
    TEST_ASSERT_EQ(page.next, 1 + 3 * STAND_INDEX_PAGE, "and the rest begin past the page");
    TestEvidence("steps the dearest hold of the index's lock gave the query:", most);
    TEST_ASSERT(most <= TagBitmapQueryHoldBound(0),
                "no hold gave the query more than its steps and the ask that crossed them");
out:
    kfree(ids);
    tag_bitmap_destroy(idx);
    return verdict;
}

// ----------------------------------------------------------------------------
// The registry's hashes grow with its tags
// ----------------------------------------------------------------------------

static TestResult TestStandIndexRegistryRehash(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = kmalloc(sizeof(TagRegistry));
    bool         built   = false;
    TagIdList    ids     = { NULL, 0, 0 };
    char         text[24];

    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;

    for (uint32_t i = 0; i < STAND_INDEX_TAGS; i++) {
        uint32_t    id;
        TagSpelling one;
        ksnprintf(text, sizeof(text), "many:%u", i);
        TEST_ASSERT_OK(TagSpell(text, strlen(text), &one), "a tag of the key many");
        TEST_ASSERT_OK(TagRegistryIntern(scratch, &one, &id), "is interned");
        ksnprintf(text, sizeof(text), "key%u", i);
        TEST_ASSERT_OK(TagSpellKey(text, strlen(text), &one), "a key of its own");
        TEST_ASSERT_OK(TagRegistryIntern(scratch, &one, &id), "is interned");
    }
    TEST_ASSERT(scratch->bucket_count >= scratch->total_tags,
                "the hash of tags has grown to a bucket a tag");
    TEST_ASSERT(scratch->key_bucket_count >= STAND_INDEX_TAGS + 1u,
                "and the hash of keys to a bucket a key");

    for (uint32_t i = 0; i < STAND_INDEX_TAGS; i++) {
        TagSpelling one;
        ksnprintf(text, sizeof(text), "many:%u", i);
        TEST_ASSERT_OK(TagSpell(text, strlen(text), &one), "the tag spells again");
        TEST_ASSERT(TagRegistryLookup(scratch, &one) != TAGFS_INVALID_TAG_ID,
                    "every tag is found after the hash moved");
    }
    TEST_ASSERT_OK(TagRegistryKeyIds(scratch, "many", 4, &ids), "the key's tags are asked for");
    TEST_ASSERT_EQ(ids.count, STAND_INDEX_TAGS, "and every one of them answers to its key");
out:
    kfree(ids.ids);
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    return verdict;
}

// ----------------------------------------------------------------------------
// More files than the old ceiling, asked a page at a time
// ----------------------------------------------------------------------------

/* storage.query everywhere for `text`, the answer into s_crate with room for
 * `room` ids: the handler's answer. */
static int StandIndexDeckQuery(const char* text, uint32_t room, uint64_t* out_size)
{
    const OpRegistration* reg   = OpRegistryLookup(OP_KIND(DECK_STORAGE, STORAGE_TAG_QUERY));
    ManifestOp*           op    = (ManifestOp*)s_query_op;
    uint8_t               scope = STORAGE_SCOPE_EVERYWHERE;

    *out_size = 0;
    if (!reg)
        return ERR_NOT_INITIALIZED;
    memset(s_query_op, 0, sizeof(s_query_op));
    op->op_kind    = OP_KIND(DECK_STORAGE, STORAGE_TAG_QUERY);
    op->param_size = 1;
    op->in_crate   = 0;
    op->out_crate  = 1;
    memcpy(op->params, &scope, 1);
    s_query_crates[0] = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)text,
                                 .size = strlen(text), .capacity = strlen(text) };
    s_query_crates[1] = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)s_crate,
                                 .capacity = 8 + (uint64_t)room * sizeof(uint64_t) };
    int rc = reg->handler(op, s_query_crates, 2, NULL);
    *out_size = s_query_crates[1].size;
    return rc;
}

/* Every page of the question, walked: each id the next of ours, in order. */
static bool StandIndexWalkQuery(const TagSpelling* name, uint32_t made, uint32_t* out_seen)
{
    TagFSPage page;
    uint64_t  from = 1;
    uint32_t  seen = 0;
    do {
        if (TagFSQueryPage(name, 1, from, s_page, STAND_INDEX_PAGE, seen == 0, &page) != OK)
            return false;
        if (seen == 0 && page.total != made)
            return false;
        for (uint32_t k = 0; k < page.delivered; k++) {
            if (seen >= made || s_page[k] != s_made[seen])
                return false;
            seen++;
        }
        from = page.next;
    } while (from != 0);
    *out_seen = seen;
    return true;
}

/* Every page of the list, walked: all of ours among the volume's, in order. */
static bool StandIndexWalkList(uint32_t made, uint32_t* out_found)
{
    TagFSPage page;
    uint64_t  from  = 1;
    uint32_t  found = 0;
    uint64_t  last  = 0;
    do {
        if (TagFSListPage(from, s_page, STAND_INDEX_PAGE, false, &page) != OK)
            return false;
        for (uint32_t k = 0; k < page.delivered; k++) {
            if (s_page[k] <= last)
                return false;
            last = s_page[k];
            if (found < made && s_page[k] == s_made[found])
                found++;
        }
        from = page.next;
    } while (from != 0);
    *out_found = found;
    return true;
}

static TestResult TestStandIndexCursorPast1024(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    made    = 0;
    uint32_t    seen    = 0;
    uint64_t    said    = 0;
    uint32_t    head[2] = { 0, 0 };   /* the answer's delivered and total */
    const char* name    = "stand_index_many";
    TagSpelling spelled;

    for (; made < STAND_INDEX_MANY; made++)
        TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, &s_made[made]), 0,
                       "a file of the name is made");
    TEST_ASSERT_OK(TagSpellKey(name, strlen(name), &spelled), "the name spells as a key");

    TEST_ASSERT(StandIndexWalkQuery(&spelled, made, &seen),
                "the question's pages give every file of the name once, in id order, and "
                "its first page counts them all");
    TEST_ASSERT_EQ(seen, STAND_INDEX_MANY, "all 1100 of them, past the 1024 a query once held");
    TEST_ASSERT(StandIndexWalkList(made, &seen), "the list's pages rise through the volume");
    TEST_ASSERT_EQ(seen, STAND_INDEX_MANY, "and carry every one of the 1100");

    TEST_ASSERT_OK(StandIndexDeckQuery(name, 16, &said), "storage.query with room for 16");
    memcpy(head, s_crate, sizeof(head));
    TEST_ASSERT(head[0] == 16 && head[1] == STAND_INDEX_MANY,
                "fills its room and says the whole count");
    TEST_ASSERT_EQ(said, 8 + 16 * sizeof(uint64_t), "the crate holds what it says it holds");
    TEST_ASSERT_OK(StandIndexDeckQuery(name, STAND_INDEX_CRATE, &said), "storage.query with room for all");
    memcpy(head, s_crate, sizeof(head));
    TEST_ASSERT(head[0] == STAND_INDEX_MANY && head[1] == STAND_INDEX_MANY,
                "delivers every file of the name");
    for (uint32_t k = 0; k < STAND_INDEX_MANY; k++)
        TEST_ASSERT_EQ(s_crate[1 + k], s_made[k], "each in id order, across the pieces it is written in");
out:
    for (uint32_t k = 0; k < made; k++)
        tagfs_delete_file(s_made[k], INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// The last file id (step 1.4)
// ----------------------------------------------------------------------------

#define STAND_INDEX_FID_MUTANT 0xFFFFFFF0u   /* the design's mutant: sixteen ids before the end */
#define STAND_INDEX_FID_ASKS   17u           /* every id left, and two asks past the last */

/*
 * The issuer is asked on a copy of the Ledger whose counter stands at
 * 0xFFFFFFF0: fifteen ids come out, each the one after the last, the last
 * 0xFFFFFFFE, and every ask after that is refused without the counter moving
 * — a counter at 0, the one an earlier kernel let wrap, is refused too. Then
 * the volume's own create is asked with its counter at the end, and must
 * refuse the same way and make nothing. The fifteen are not made as files: a
 * file table row is placed by its id (file_table.c), and one at 0xFFFFFFF0
 * would ask the table's chain for eight million blocks at the next cycle. The
 * name has no stem, so the create interns no tag and writes nothing before it
 * reaches the counter, and the live counter is at the end for no longer than
 * that.
 */
static TestResult TestStandIndexFidExhausted(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    VolumeLedger copy;
    uint32_t     issued  = 0, refused = 0, never = 0;
    uint64_t     fid = 0, last = 0;
    uint32_t     saved   = 0, left_at = 0;
    uint64_t     made = 0;
    bool         moved   = false;
    int          rc      = -1;
    VolumeTally  before;
    VolumeTally  after;

    memset(&copy, 0, sizeof(copy));
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    copy.next_file_id = STAND_INDEX_FID_MUTANT;
    for (uint32_t ask = 0; ask < STAND_INDEX_FID_ASKS; ask++) {
        error_t got = LedgerIssueFileId(&copy, &fid);
        if (got != OK) {
            TEST_ASSERT_EQ(got, ERR_TAGFS_FID_EXHAUSTED, "an issuer out of ids says so");
            refused++;
            continue;
        }
        if (fid == 0 || fid == UINT32_MAX || (issued && fid != last + 1u))
            never++;
        last = fid;
        issued++;
    }
    TestEvidence("ids issued that must never be — zero, the one that wraps the counter, "
                 "or one out of turn", never);
    if (never) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(issued, 15, "fifteen ids are issued from 0xFFFFFFF0");
    TEST_ASSERT_EQ(last, UINT32_MAX - 1u, "the last of them 0xFFFFFFFE");
    TEST_ASSERT_EQ(refused, 2, "and every ask after it is refused");
    TEST_ASSERT_EQ(copy.next_file_id, UINT32_MAX, "with the counter left at the end");
    copy.next_file_id = 0;
    TEST_ASSERT_EQ(LedgerIssueFileId(&copy, &fid), ERR_TAGFS_FID_EXHAUSTED,
                   "a counter an earlier kernel let wrap to 0 is refused too");
    TEST_ASSERT_EQ(copy.next_file_id, 0, "and left where it was");

    TEST_ASSERT(VolumeTallyTake(&before), "the volume is counted");
    TagFSLockTake(&fs->lock);
    saved                   = fs->ledger.next_file_id;
    fs->ledger.next_file_id = UINT32_MAX;
    moved                   = true;
    TagFSLockDrop(&fs->lock);
    rc = tagfs_create_file(".stand_fid", NULL, 0, &made);
    TagFSLockTake(&fs->lock);
    left_at                 = fs->ledger.next_file_id;
    fs->ledger.next_file_id = saved;
    moved                   = false;
    TagFSLockDrop(&fs->lock);

    TestEvidence("files made on a volume that had issued every id", rc == 0);
    if (rc == 0) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, -(int)ERR_TAGFS_FID_EXHAUSTED,
                   "a create on a volume that has issued every id is refused as such");
    TEST_ASSERT_EQ(left_at, UINT32_MAX, "and the volume's counter did not move");
    TEST_ASSERT(VolumeTallyTake(&after), "the volume is counted again");
    TEST_ASSERT_EQ(after.files, before.files, "no file was made");
    TEST_ASSERT_EQ(after.held, before.held, "and no block was taken");
out:
    if (moved) {
        TagFSLockTake(&fs->lock);
        fs->ledger.next_file_id = saved;
        TagFSLockDrop(&fs->lock);
    }
    if (rc == 0)
        tagfs_delete_file(made, INK_UNWARRANTED);
    return verdict;
}

static const TestCase g_stand_index_tests[] = {
    { .name  = "stand_index_map_costs_the_live", .func = TestStandIndexMapCostsTheLive,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "memory for the files is as long as the highest file id the volume ever "
                       "issued, the dead ones included" },
    { .name  = "stand_index_window_edge", .func = TestStandIndexWindowEdge,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a tag's members are lost or misplaced where one window of 65536 ids "
                       "meets the next, or where a window turns bitmap or array" },
    { .name  = "stand_index_query_hold_bounded", .func = TestStandIndexQueryHoldBounded,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a query asks every tag of a wildcard in one step of its hold, so a key "
                       "with thousands of values holds the index's lock, and interrupts, for "
                       "thousands of asks a step" },
    { .name  = "stand_index_registry_rehash", .func = TestStandIndexRegistryRehash,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "the registry's hashes keep a fixed count of buckets however many tags "
                       "it holds" },
    { .name  = "stand_index_cursor_past_1024", .func = TestStandIndexCursorPast1024,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a question of which files there are is answered into an array cut at a "
                       "ceiling, and the files past it are not there" },
    { .name  = "stand_index_fid_exhausted", .func = TestStandIndexFidExhausted,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "the 2^32-th create is given file id 0 and the one after it id 1, a "
                       "file the volume already holds",
      .format = DEED_FORMAT_V1, .analog = "stand_errata_wide_fid" },
};

const TestSuite g_stand_index_suite = {
    .tests = g_stand_index_tests, .count = ARRAY_SIZE(g_stand_index_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
