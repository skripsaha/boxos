#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../keep/keep.h"
#include "../settle/settle.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_name suite: a file's name is a field of its card, found by the
// name index (file_map/name_index.h)
//
// A word finds a file when it is the whole of the file's name or its stem, and
// no tag comes of it. The index is derived and kept in the one place every
// create, rename, delete, keep, restore, let-go and undo passes — the map
// taking or letting go a record — so what is asked here is that it agrees with
// a walk of every card after each of them, and after a mount reads the volume
// again; and, on a map of the suite's own, that it holds as many files as the
// table was first made for and many times more, a record replaced by another
// of its name never leaving a link behind.
// ============================================================================

#define STAND_NAME_PAGE   16u
#define STAND_NAME_GROWN  3000u      /* files of the map the table must grow for */
#define STAND_NAME_SAMPLE 37u        /* every one of them looked for by its word */

/* How many files the volume says are called `word`, with `tags` worn and
 * `without` set aside; *out_first the least id among them (0 for none). The
 * count asked of the first page alone. */
static uint64_t StandNamedCount(TagFSVolume* vol, const char* word, const TagSpelling* tags, uint32_t tag_count,
                                const TagSpelling* without, uint32_t without_count, uint64_t* out_first)
{
    uint64_t  ids[STAND_NAME_PAGE];
    TagFSPage page;
    error_t   rc = TagFSNamedPage(vol, TagOwnerPerson(), word, strlen(word), tags, tag_count, without, without_count, 1, ids,
                                  STAND_NAME_PAGE, true, &page);
    if (out_first)
        *out_first = rc == OK && page.delivered ? ids[0] : 0;
    return rc == OK ? page.total : UINT64_MAX;
}

static uint64_t StandNamed(TagFSVolume* vol, const char* word)
{
    return StandNamedCount(vol, word, NULL, 0, NULL, 0, NULL);
}

/* Whether the pages of the question for `word`, room for `room` ids each, hold
 * the files `want` (`count` of them), each once, in file id order. */
static bool StandNamedPages(TagFSVolume* vol, const char* word, uint32_t room, const uint64_t* want, uint32_t count)
{
    uint64_t  ids[STAND_NAME_PAGE];
    uint64_t  from = 1;
    uint32_t  seen = 0;
    TagFSPage page;
    do {
        if (TagFSNamedPage(vol, TagOwnerPerson(), word, strlen(word), NULL, 0, NULL, 0, from, ids, room, false, &page) != OK)
            return false;
        for (uint32_t i = 0; i < page.delivered; i++)
            if (seen >= count || ids[i] != want[seen++])
                return false;
        from = page.next;
    } while (from != 0);
    return seen == count;
}

static void StandNameLetGo(TagFSVolume* vol, uint64_t fid)
{
    if (fid)
        tagfs_delete_file(vol, fid, INK_WARRANTED);
}

/* Whether file `fid` answers `word` as a card says it: the whole of its name
 * or its stem, and it is not kept deleted. */
static bool StandCardAnswers(TagFSVolume* vol, uint64_t fid, const char* word, size_t length)
{
    FileRecord* rec = FileMapRecordHold(vol->state.files, fid, NULL);
    bool        yes = false;
    size_t      lengths[2];
    if (!rec)
        return false;
    uint32_t words = TagFSNameWords(rec->name, rec->name_len, lengths);
    for (uint32_t i = 0; i < words && !(rec->flags & TAGFS_FILE_DELETED); i++)
        yes |= lengths[i] == length && memcmp(rec->name, word, length) == 0;
    FileRecordRelease(rec);
    return yes;
}

/* How many of the live files the cards say answer `word`. */
static uint64_t StandCardsAnswering(TagFSVolume* vol, const char* word, size_t length)
{
    uint64_t files[STAND_NAME_PAGE], count = 0, from = 1;
    uint32_t n;
    while ((n = FileMapFiles(vol->state.files, from, files, ARRAY_SIZE(files))) > 0) {
        for (uint32_t i = 0; i < n; i++)
            count += StandCardAnswers(vol, files[i], word, length);
        from = files[n - 1] + 1;
    }
    return count;
}

bool StandNamesExact(TagFSVolume* vol)
{
    TagFSState* fs      = &vol->state;
    uint64_t    files[STAND_NAME_PAGE];
    uint64_t    from    = 1;
    uint32_t    n, links = 0;
    bool        exact   = true;

    if (!tagfs_enter(vol))
        return false;
    while (exact && (n = FileMapFiles(fs->files, from, files, ARRAY_SIZE(files))) > 0) {
        for (uint32_t i = 0; exact && i < n; i++) {
            FileRecord* rec = FileMapRecordHold(fs->files, files[i], NULL);
            if (!rec)
                continue;
            size_t   lengths[2];
            uint32_t words = TagFSNameWords(rec->name, rec->name_len, lengths);
            for (uint32_t w = 0; exact && w < words && !(rec->flags & TAGFS_FILE_DELETED); w++) {
                uint64_t  found[STAND_NAME_PAGE], at = 1, sees = 0, later = 0;
                uint64_t  expects = StandCardsAnswering(vol, rec->name, lengths[w]);
                TagFSPage page;
                links++;
                do {
                    exact = TagFSNamedPage(vol, TagOwnerPerson(), rec->name, lengths[w], NULL, 0, NULL, 0, at, found, STAND_NAME_PAGE,
                                           false, &page) == OK;
                    for (uint32_t k = 0; exact && k < page.delivered; k++) {
                        exact = StandCardAnswers(vol, found[k], rec->name, lengths[w]) && found[k] > later;
                        later = found[k];
                        sees++;
                    }
                    at = page.next;
                } while (exact && at != 0);
                exact = exact && sees == expects;
            }
            FileRecordRelease(rec);
        }
        from = files[n - 1] + 1;
    }
    exact = exact && FileMapNameLinks(fs->files) == links;
    tagfs_leave(vol);
    return exact;
}

// ----------------------------------------------------------------------------
// Create, rename, delete: the words a name answers to
// ----------------------------------------------------------------------------

static TestResult TestStandNameFindsByWholeNameOrStem(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint64_t    a = 0, b = 0, hidden = 0, plain = 0, tagged = 0, lengthy = 0, first = 0;
    char        lengthy_name[300 + 5];
    char        lengthy_stem[300 + 1];
    TagSpelling tag, other;

    TEST_ASSERT(StandMakeFile(vol, "stand_nm_slot3.sav", NULL, 0, &a), "a file called stand_nm_slot3.sav");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot3"), 1u, "its stem finds it");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot3.sav"), 1u, "and its whole name");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot"), 0u, "a part of the stem is no word of it");
    TEST_ASSERT_EQ(StandNamed(vol, "sav"), 0u, "nor is its extension");
    TEST_ASSERT_EQ(StandNamed(vol, "STAND_NM_SLOT3"), 0u, "the bytes are exact: another case is another word");
    TEST_ASSERT_EQ(tagfs_tag_lookup(vol, "stand_nm_slot3"), TAGFS_INVALID_TAG_ID, "and the name made no tag");
    TEST_ASSERT_EQ(tagfs_tag_lookup(vol, "stand_nm_slot3.sav"), TAGFS_INVALID_TAG_ID, "of its stem or its whole");

    TEST_ASSERT(StandMakeFile(vol, "stand_nm_slot3.old", NULL, 0, &b), "a second file with the same stem");
    TEST_ASSERT_EQ(StandNamedCount(vol, "stand_nm_slot3", NULL, 0, NULL, 0, &first), 2u, "two files are called the stem");
    TEST_ASSERT_EQ(first, a, "the least id first");
    uint64_t both[2] = { a, b };
    TEST_ASSERT(StandNamedPages(vol, "stand_nm_slot3", 1, both, 2), "a page of one id each gives them in id order");
    TEST_ASSERT(StandNamedPages(vol, "stand_nm_slot3", STAND_NAME_PAGE, both, 2), "and a page of many gives them too");

    TEST_ASSERT_EQ(tagfs_rename_file(vol, a, "stand_nm_slot4.sav"), 0, "the first is renamed");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot3"), 1u, "the old stem finds only the other");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot3.sav"), 0u, "the old whole name finds none");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot4"), 1u, "the new stem finds it");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot4.sav"), 1u, "and the new whole name");
    TEST_ASSERT_EQ(tagfs_tag_lookup(vol, "stand_nm_slot4"), TAGFS_INVALID_TAG_ID, "and the rename made no tag");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");

    TEST_ASSERT(StandMakeFile(vol, ".stand_nm_hidden", NULL, 0, &hidden), "a file whose only dot begins its name");
    TEST_ASSERT_EQ(StandNamed(vol, ".stand_nm_hidden"), 1u, "is found by its whole name");
    TEST_ASSERT_EQ(StandNamed(vol, ""), UINT64_MAX, "and no word of no bytes finds anything: it is refused");
    TEST_ASSERT(StandMakeFile(vol, "stand_nm_plain", NULL, 0, &plain), "a file with no dot");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_plain"), 1u, "is found by its name, once");

    TEST_ASSERT_OK(TagSpell("stand_nm_label", 14u, &tag), "a tag is spelled");
    TEST_ASSERT_OK(TagSpell("stand_nm_other", 14u, &other), "and another");
    TEST_ASSERT(StandMakeTaggedFile(vol, "stand_nm_slot3.tag", "stand_nm_label", NULL, 0, &tagged), "a tagged file");
    TEST_ASSERT_EQ(StandNamedCount(vol, "stand_nm_slot3", &tag, 1, NULL, 0, &first), 1u,
                   "a word and a tag find the files called so that wear it");
    TEST_ASSERT_EQ(first, tagged, "that one");
    TEST_ASSERT_EQ(StandNamedCount(vol, "stand_nm_slot3", &other, 1, NULL, 0, NULL), 0u, "and none of a tag nobody wears");
    TEST_ASSERT_EQ(StandNamedCount(vol, "stand_nm_slot3", NULL, 0, &tag, 1, NULL), 1u,
                   "set aside, the tag leaves its file out of the answer");

    memset(lengthy_name, 'n', 300);
    memcpy(lengthy_name + 300, ".txt", 5);
    memset(lengthy_stem, 'n', 300);
    lengthy_stem[300] = '\0';
    TEST_ASSERT(StandMakeFile(vol, lengthy_name, NULL, 0, &lengthy), "a file whose name is 300 bytes and a stem");
    TEST_ASSERT_EQ(StandNamed(vol, lengthy_stem), 1u, "is found by that stem: a name is no key, and no width cuts it");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");

    TEST_ASSERT_EQ(tagfs_delete_file(vol, a, INK_UNWARRANTED), 0, "the renamed file is deleted");
    a = 0;
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_slot4"), 0u, "no word of it finds anything");
    TEST_ASSERT(StandNamesExact(vol), "and the index agrees with every card");
out:
    StandNameLetGo(vol, a);
    StandNameLetGo(vol, b);
    StandNameLetGo(vol, hidden);
    StandNameLetGo(vol, plain);
    StandNameLetGo(vol, tagged);
    StandNameLetGo(vol, lengthy);
    return verdict;
}

// ----------------------------------------------------------------------------
// Kept, brought back, undone, let go, and a mount
// ----------------------------------------------------------------------------

#define STAND_NAME_TERM_S 3600ull

static TestResult TestStandNameFollowsKeepRestoreUndo(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    uint64_t   fid = 0;
    InkReceipt receipt;

    TEST_ASSERT(StandMakeFile(vol, "stand_nm_kept.dat", NULL, 0, &fid), "a file called stand_nm_kept.dat");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 1u, "found by its stem");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, fid, STAND_NAME_TERM_S, INK_WARRANTED), 0, "it is deleted for a term");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 0u, "no question meets a file kept deleted: not by its stem");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept.dat"), 0u, "nor by its whole name");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, fid, 2u * STAND_NAME_TERM_S, INK_WARRANTED), 0, "its term is moved");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 0u, "and it is still not met");
    TEST_ASSERT_EQ(tagfs_restore_file(vol, fid, INK_WARRANTED), 0, "it is brought back");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 1u, "and found again, by its stem");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept.dat"), 1u, "and by its whole name");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");

    TEST_ASSERT_OK(InkUndone(vol, fid, &receipt), "its last write is rolled back, as a mount's verdict does");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 1u, "an undone file is the file it was: still found");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");

    TEST_ASSERT_EQ(tagfs_keep_file(vol, fid, STAND_NAME_TERM_S, INK_WARRANTED), 0, "it is deleted for a term again");
    TEST_ASSERT_EQ(KeepSweepNow(vol, KeepNowNs() + (2u * STAND_NAME_TERM_S) * 1000000000ull), 1u,
                   "and a sweep past the term lets it go");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_kept"), 0u, "no word of it finds anything");
    TEST_ASSERT(StandNamesExact(vol), "and the index agrees with every card");
    fid = 0;
out:
    StandNameLetGo(vol, fid);
    return verdict;
}

static TestResult TestStandNameSurvivesAMount(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    uint64_t   live = 0, trashed = 0, renamed = 0;

    TEST_ASSERT(StandMakeFile(vol, "stand_nm_mount.dat", NULL, 0, &live), "a file called stand_nm_mount.dat");
    TEST_ASSERT(StandMakeFile(vol, "stand_nm_trash.dat", NULL, 0, &trashed), "another");
    TEST_ASSERT(StandMakeFile(vol, "stand_nm_before.dat", NULL, 0, &renamed), "a third");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, trashed, STAND_NAME_TERM_S, INK_WARRANTED), 0, "the second is deleted for a term");
    TEST_ASSERT_EQ(tagfs_rename_file(vol, renamed, "stand_nm_after.dat"), 0, "the third is renamed");
    TEST_ASSERT_EQ(TagFSSettle(vol), OK, "the changes are settled");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh: the index is built from the cards it reads");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_mount"), 1u, "the first is found");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_trash"), 0u, "the one kept deleted is not");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_before"), 0u, "the third is not found by its old name");
    TEST_ASSERT_EQ(StandNamed(vol, "stand_nm_after"), 1u, "but by its new one");
    TEST_ASSERT(StandNamesExact(vol), "the index agrees with every card");
out:
    StandNameLetGo(vol, live);
    StandNameLetGo(vol, trashed);
    StandNameLetGo(vol, renamed);
    return verdict;
}

// ----------------------------------------------------------------------------
// A map of the suite's own: the table grows, a record put again leaves no link
// ----------------------------------------------------------------------------

static char s_names[STAND_NAME_GROWN][20];

/* A record of file `k` of the map, called `name`, wearing the tag given. */
static bool StandNamePut(FileMap* map, uint32_t k, const char* name, uint32_t tag)
{
    uint64_t      fid  = 1 + k;
    TagFSMetadata meta = { .file_id = fid, .flags = TAGFS_FILE_ACTIVE, .filename = (char*)name,
                           .tag_ids = &tag, .tag_count = 1 };
    FileRecord*   rec   = FileRecordBuild(&meta);
    FileMapStake  stake = { .count = 0 };
    RecordWhere   where = { .gen = 0, .shape = 0 }, was;
    if (!rec || FileMapStakeFor(map, fid, &stake) != OK) {
        FileRecordRelease(rec);
        return false;
    }
    error_t put = FileMapRecordPut(map, fid, rec, &where, &stake, &was);
    FileMapStakeLetGo(&stake);
    return put == OK;
}

/* How many files the map says are called `word`, the first of them in *first. */
static uint64_t StandNameMapCount(FileMap* map, const char* word, uint64_t* first)
{
    uint64_t ids[STAND_NAME_PAGE], next, count = 0, from = 1;
    *first = 0;
    do {
        uint32_t n = FileMapNamed(map, word, strlen(word), from, ids, STAND_NAME_PAGE, &next);
        if (n && !*first)
            *first = ids[0];
        count += n;
        from = next;
    } while (from != 0);
    return count;
}

static TestResult TestStandNameMapGrowsAndHoldsNoStaleLink(void)
{
    TestResult verdict = TEST_PASS;
    FileMap*   map     = FileMapCreate();
    uint64_t   first   = 0;

    TEST_ASSERT(map != NULL, "a map of the test's own");
    for (uint32_t k = 0; k < STAND_NAME_GROWN; k++) {
        ksnprintf(s_names[k], sizeof(s_names[k]), "grown%u.dat", k);
        TEST_ASSERT(StandNamePut(map, k, s_names[k], 1), "a file is put in the map");
    }
    TEST_ASSERT_EQ(FileMapNameLinks(map), 2u * STAND_NAME_GROWN, "two links a file: its whole name and its stem");
    for (uint32_t k = 0; k < STAND_NAME_GROWN; k += STAND_NAME_SAMPLE) {
        char stem[20];
        ksnprintf(stem, sizeof(stem), "grown%u", k);
        TEST_ASSERT_EQ(StandNameMapCount(map, stem, &first), 1u, "a word finds its file, after the table grew");
        TEST_ASSERT_EQ(first, 1u + k, "and that file");
        TEST_ASSERT_EQ(StandNameMapCount(map, s_names[k], &first), 1u, "and so does the whole name");
    }

    for (uint32_t k = 0; k < STAND_NAME_GROWN; k++)
        TEST_ASSERT(StandNamePut(map, k, s_names[k], 2), "a record of the same name replaces each");
    TEST_ASSERT_EQ(FileMapNameLinks(map), 2u * STAND_NAME_GROWN, "and leaves no link behind");

    for (uint32_t k = 0; k < STAND_NAME_GROWN; k += 2) {
        ksnprintf(s_names[k], sizeof(s_names[k]), "moved%u.dat", k);
        TEST_ASSERT(StandNamePut(map, k, s_names[k], 2), "half of them are put again under another name");
    }
    TEST_ASSERT_EQ(FileMapNameLinks(map), 2u * STAND_NAME_GROWN, "the links moved with the names");
    TEST_ASSERT_EQ(StandNameMapCount(map, "grown0", &first), 0u, "the old stem finds none");
    TEST_ASSERT_EQ(StandNameMapCount(map, "moved0", &first), 1u, "the new one finds it");

    for (uint32_t k = 0; k < STAND_NAME_GROWN; k += 3)
        FileMapDrop(map, 1 + k, NULL);
    uint32_t dropped = (STAND_NAME_GROWN + 2u) / 3u;
    TEST_ASSERT_EQ(FileMapNameLinks(map), 2u * (STAND_NAME_GROWN - dropped), "a file let go takes its links");
    TEST_ASSERT_EQ(StandNameMapCount(map, "grown3", &first), 0u, "and is found by none of its words");
    for (uint32_t k = 0; k < STAND_NAME_GROWN; k++)
        FileMapDrop(map, 1 + k, NULL);
    TEST_ASSERT_EQ(FileMapNameLinks(map), 0u, "and with every file let go, none is left");
out:
    FileMapDestroy(map);
    return verdict;
}

/* Names are not unique: a volume may hold tens of thousands of files of one
 * name, all in one chain of the index. Making them, putting their records
 * again and letting them go must not cost a walk of the chain each. */
static TestResult TestStandNameManyOfOneName(void)
{
    TestResult verdict = TEST_PASS;
    FileMap*   map     = FileMapCreate();
    uint64_t   first   = 0;

    TEST_ASSERT(map != NULL, "a map of the test's own");
    for (uint32_t k = 0; k < STAND_NAME_GROWN; k++)
        TEST_ASSERT(StandNamePut(map, k, "same.dat", 1), "a file called same.dat is put in the map");
    TEST_ASSERT_EQ(StandNameMapCount(map, "same", &first), STAND_NAME_GROWN, "the stem finds them all");
    TEST_ASSERT_EQ(first, 1u, "the least id first");
    for (uint32_t k = STAND_NAME_GROWN; k-- > 0;)
        TEST_ASSERT(StandNamePut(map, k, "same.dat", 2), "a record of each is put again, the last first");
    TEST_ASSERT_EQ(FileMapNameLinks(map), 2u * STAND_NAME_GROWN, "and no link is left behind");
    TEST_ASSERT_EQ(StandNameMapCount(map, "same.dat", &first), STAND_NAME_GROWN, "the whole name finds them all");
    for (uint32_t k = 0; k < STAND_NAME_GROWN; k += 2)
        FileMapDrop(map, 1 + k, NULL);
    TEST_ASSERT_EQ(StandNameMapCount(map, "same", &first), STAND_NAME_GROWN / 2u, "half let go, half are found");
    TEST_ASSERT_EQ(first, 2u, "from the least id left");
out:
    FileMapDestroy(map);
    return verdict;
}

static const TestCase g_stand_name_tests[] = {
    { .name = "stand_name_finds_by_whole_name_or_stem", .func = TestStandNameFindsByWholeNameOrStem,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_name_follows_keep_restore_undo", .func = TestStandNameFollowsKeepRestoreUndo,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_name_survives_a_mount", .func = TestStandNameSurvivesAMount,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_name_map_grows_and_holds_no_stale_link", .func = TestStandNameMapGrowsAndHoldsNoStaleLink,
      .reach = TEST_READS_ONLY },
    { .name = "stand_name_many_of_one_name", .func = TestStandNameManyOfOneName,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_name_suite = {
    g_stand_name_tests, sizeof(g_stand_name_tests) / sizeof(g_stand_name_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
