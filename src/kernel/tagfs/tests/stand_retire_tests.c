#include "tests.h"
#include "stand_tests.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../keep/keep.h"
#include "../retire/retire.h"
#include "../settle/settle.h"
#include "../settle/endpaper_fact.h"
#include "../tag_registry/tag_registry.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_retire suite: a program left, and its own tags left with it
// (retire/retire.h)
//
// A program's own tags are its labels for itself, and the program is the file
// it was started from. The tests make a "program" of a file, put tags of its
// own on several files — live ones and one kept deleted — and a person's tag
// of the same text on one of them, then let the program go in every way the
// volume lets a file go: every tag it owned comes off every file and is
// RETIRED, the person's tag of the same text is the person's still, and the
// id is not given again — before and after a mount, whether the mount reads it
// off the ribbon or off a Tally, and when a cut came before the duty did.
// ============================================================================

#define STAND_RETIRE_TERM_S  3600ull

static const TagSpelling s_save  = { .key = "save",  .key_len = 4 };
static const TagSpelling s_level = { .key = "level", .value = "3", .key_len = 5, .value_len = 1 };

/* The program and the three files that wear its tags: two live, one deleted
 * and kept. */
typedef struct {
    uint64_t program;
    uint64_t files[3];
    uint32_t ids[2];     /* the program's `save` and `level:3` */
    uint32_t person;     /* the person's `save`, worn by files[0] */
} StandRetire;

static uint32_t StandRetireId(uint64_t program, const TagSpelling* tag)
{
    return TagRegistryLookup(tagfs_get_state()->registry, TagOwnerProgram(program), tag);
}

static bool StandRetireWorn(uint64_t file, uint32_t tag)
{
    TagFSMetadata md;
    bool          worn = false;
    if (tagfs_get_metadata(file, &md) != 0)
        return false;
    for (uint32_t i = 0; i < md.tag_count; i++)
        worn = worn || md.tag_ids[i] == tag;
    tagfs_metadata_free(&md);
    return worn;
}

static bool StandRetireWornAnywhere(const StandRetire* r, uint32_t tag)
{
    for (uint32_t i = 0; i < 3; i++)
        if (StandRetireWorn(r->files[i], tag))
            return true;
    return false;
}

/* The program, the files, and every tag put on: false when a step would not. */
static bool StandRetireMake(StandRetire* r, uint32_t n)
{
    static const char* const names[] = { "retire_a.dat", "retire_b.dat", "retire_c.dat" };
    memset(r, 0, sizeof(*r));
    if (!StandMakeFile("retire_prog.bin", "prog", 4, &r->program))
        return false;
    for (uint32_t i = 0; i < 3; i++) {
        if (!StandMakeFile(names[i], "data", 4, &r->files[i]))
            return false;
        if (i < n && (tagfs_add_tag_string(TagOwnerProgram(r->program), r->files[i], &s_save) != OK ||
                      tagfs_add_tag_string(TagOwnerProgram(r->program), r->files[i], &s_level) != OK))
            return false;
    }
    if (tagfs_add_tag_string(TagOwnerPerson(), r->files[0], &s_save) != OK)
        return false;
    r->ids[0]  = StandRetireId(r->program, &s_save);
    r->ids[1]  = StandRetireId(r->program, &s_level);
    r->person = TagRegistryLookup(tagfs_get_state()->registry, TagOwnerPerson(), &s_save);
    return r->ids[0] != TAGFS_INVALID_TAG_ID && r->ids[1] != TAGFS_INVALID_TAG_ID &&
           r->person != TAGFS_INVALID_TAG_ID && r->person != r->ids[0] &&
           tagfs_keep_file(r->files[2], STAND_RETIRE_TERM_S, INK_WARRANTED) == 0;
}

static void StandRetireClean(const StandRetire* r)
{
    for (uint32_t i = 0; i < 3; i++)
        (void)tagfs_delete_file(r->files[i], INK_WARRANTED);
    (void)tagfs_delete_file(r->program, INK_WARRANTED);
}

/* What the book says of the program's tags, held to one set of questions
 * wherever the registry came from: both retired, found by no lookup, worn by
 * none of the files, the person's `save` the person's still and worn by
 * the file that wore it, and a program's tag of the same text given another
 * id. */
static bool StandRetireHolds(const StandRetire* r)
{
    TagRegistry* reg = tagfs_get_state()->registry;
    TagSpelling  sp;
    for (uint32_t i = 0; i < 2; i++)
        if (!TagRegistryRetiredId(reg, r->ids[i]) || TagRegistrySpelling(reg, r->ids[i], &sp, NULL, NULL) ||
            StandRetireWornAnywhere(r, r->ids[i]))
            return false;
    return StandRetireId(r->program, &s_save) == TAGFS_INVALID_TAG_ID &&
           StandRetireId(r->program, &s_level) == TAGFS_INVALID_TAG_ID &&
           !TagRegistryRetiredId(reg, r->person) && StandRetireWorn(r->files[0], r->person);
}

/* A new tag of the gone program's text is another id, which the program's
 * absence owes to retirement in its turn. */
static bool StandRetireNeverAgain(const StandRetire* r)
{
    uint32_t again = TAGFS_INVALID_TAG_ID;
    if (TagRegistryIntern(tagfs_get_state()->registry, TagOwnerProgram(r->program), &s_save, &again) != OK)
        return false;
    return again != r->ids[0] && again != r->ids[1] && again > r->ids[1];
}

static TestResult TestStandRetireProgramGone(void)
{
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    TEST_ASSERT(StandRetireMake(&r, 3), "a program, three files — one kept deleted — and tags of its own on all of them");
    TEST_ASSERT(StandRetireWornAnywhere(&r, r.ids[0]) && StandRetireWorn(r.files[2], r.ids[1]),
                "the program's tags are worn, by the file kept deleted as well");
    TEST_ASSERT_OK(RetireSweepNow(&n), "a look while the program's file stands");
    TEST_ASSERT_EQ(n, 0, "retires nothing");
    TEST_ASSERT_EQ(tagfs_delete_file(r.program, INK_WARRANTED), 0, "the program's file is deleted with no term");
    TEST_ASSERT(StandRetireWorn(r.files[0], r.ids[0]), "its tags are worn still: nothing was done in the deleter's context");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks");
    TEST_ASSERT_EQ(n, 2, "and retires its two tags");
    TEST_ASSERT(StandRetireHolds(&r), "both are off every file and found by no lookup, and the person's tag of the "
                "same text is the person's still");
    TEST_ASSERT(StandRetireNeverAgain(&r), "a tag of the same text and owner is another id, never the retired one");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the tag the gone program put on since is owed in its turn");
    TEST_ASSERT_EQ(n, 1, "and retired by the next look");
    TEST_ASSERT_OK(RetireSweepNow(&n), "a look with nothing owed");
    TEST_ASSERT_EQ(n, 0, "retires nothing");
out:
    StandRetireClean(&r);
    return verdict;
}

static TestResult TestStandRetireTrashKeepsTheTags(void)
{
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    uint64_t    later = KeepNowNs() + (2u * STAND_RETIRE_TERM_S) * 1000000000ull;
    TEST_ASSERT(StandRetireMake(&r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_keep_file(r.program, STAND_RETIRE_TERM_S, INK_WARRANTED), 0, "the program's file goes to the trash");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks");
    TEST_ASSERT_EQ(n, 0, "and retires nothing: a program in the trash is not gone");
    TEST_ASSERT(StandRetireWornAnywhere(&r, r.ids[0]) && StandRetireWornAnywhere(&r, r.ids[1]) &&
                !TagRegistryRetiredId(tagfs_get_state()->registry, r.ids[0]), "its tags live");
    TEST_ASSERT_EQ(tagfs_restore_file(r.program, INK_WARRANTED), 0, "the file is brought back");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks again");
    TEST_ASSERT(n == 0 && StandRetireWorn(r.files[1], r.ids[0]) && StandRetireWorn(r.files[2], r.ids[1]) &&
                StandRetireId(r.program, &s_save) == r.ids[0], "restored, it has every tag it had, whole");
    TEST_ASSERT_EQ(tagfs_keep_file(r.program, STAND_RETIRE_TERM_S, INK_WARRANTED), 0, "it goes to the trash again");
    TEST_ASSERT(KeepSweepNow(later) >= 1u, "and its term ends: the volume lets the file go");
    TEST_ASSERT(StandRetireWornAnywhere(&r, r.ids[0]), "the tags are worn until the duty looks");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks");
    TEST_ASSERT_EQ(n, 2, "and retires them");
    TEST_ASSERT(StandRetireHolds(&r), "off every file, the kept one too");
out:
    StandRetireClean(&r);
    return verdict;
}

static TestResult TestStandRetireRefusalResumes(void)
{
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    error_t     rc;
    TEST_ASSERT(StandRetireMake(&r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(r.program, INK_WARRANTED), 0, "the program's file is deleted");
    TagFSStandShutForHalt(true);
    rc = RetireSweepNow(&n);
    TagFSStandShutForHalt(false);
    TEST_ASSERT(rc != OK && n == 0, "a volume that takes no change now refuses the duty, which retires nothing");
    TEST_ASSERT(StandRetireWornAnywhere(&r, r.ids[0]), "and takes no tag off a file");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks again once it does");
    TEST_ASSERT_EQ(n, 2, "and finishes what the refusal left: the tags closed, then taken off, then retired");
    TEST_ASSERT(StandRetireHolds(&r), "every tag off every file");
out:
    TagFSStandShutForHalt(false);
    StandRetireClean(&r);
    return verdict;
}

/* The state a retirement leaves, read where the volume was mounted afresh. */
static TestResult StandRetireAfterMount(StandRetire* r, bool tally)
{
    TestResult  verdict = TEST_PASS;
    static StandBinder b;
    const char* aside = tally ? StandBinderAside(&b) : NULL;
    uint32_t    n = 0;
    uint64_t    laid[3] = { 0 }, stood[3] = { 0 };
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRetireMake(r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(r->program, INK_WARRANTED), 0, "the program's file is deleted");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty retires its tags");
    TEST_ASSERT_EQ(n, 2, "both");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle lays the Coineds RETIRED and the files without the tags");
    if (tally) {
        TEST_ASSERT_OK(StandBinderTallies(1), "and a Tally lays TAGS with the two ids RETIRED");
        EndpaperFactMost(&laid[0], &laid[1], &laid[2]);
    }
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh");
    if (tally) {
        EndpaperFactMost(&stood[0], &stood[1], &stood[2]);
        TEST_ASSERT(laid[2] == stood[2] && laid[0] == stood[0],
                    "the mount measures TAGS and FILES as the Tally's writer did, a retired id one element of TAGS");
    }
    TEST_ASSERT(StandRetireHolds(r), "its book says the same: held as retired, found by no lookup, worn by no file");
    TEST_ASSERT_EQ(tagfs_get_state()->registry->program_tags, 0, "and no program's tag is owed");
    TEST_ASSERT(StandRetireNeverAgain(r), "no id of the book is given twice");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the tag put on since is retired");
    TEST_ASSERT_EQ(n, 1, "by the duty, as before the mount");
out:
    StandRetireClean(r);
    if (tally)
        STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandRetireRibbonReplay(void)
{
    StandRetire r;
    return StandRetireAfterMount(&r, false);
}

static TestResult TestStandRetireTallyHolds(void)
{
    StandRetire r;
    return StandRetireAfterMount(&r, true);
}

/* The mount owes the retirement of every owner whose file is gone: the cut
 * came before the duty did (`cleared` false), or after the files were cleared
 * and before the tags were retired. */
static TestResult StandRetireAfterCut(bool cleared)
{
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    TEST_ASSERT(StandRetireMake(&r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(r.program, INK_WARRANTED), 0, "the program's file is deleted");
    for (uint32_t i = 0; cleared && i < 3; i++) {
        InkReceipt receipt;
        TEST_ASSERT_OK(InkTagsOff(r.files[i], r.ids, 2, &receipt), "a file is taken off its tags by hand");
    }
    TEST_ASSERT_OK(TagFSSettle(), "the book holds the file gone, the tags in their Coineds");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh: the duty never ran");
    TEST_ASSERT_EQ(tagfs_get_state()->registry->program_tags, 2, "the registry holds the program's two tags, owed");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty the mount owes");
    TEST_ASSERT_EQ(n, 2, "retires both");
    TEST_ASSERT(StandRetireHolds(&r), "off every file, found by no lookup");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle lays them");
    TEST_ASSERT(StandStitchRemount(), "and the volume is mounted afresh again");
    TEST_ASSERT(StandRetireHolds(&r) && tagfs_get_state()->registry->program_tags == 0, "retired, none owed");
out:
    StandRetireClean(&r);
    return verdict;
}

static TestResult TestStandRetireMountSweeps(void)
{
    return StandRetireAfterCut(false);
}

static TestResult TestStandRetireMountFinishes(void)
{
    return StandRetireAfterCut(true);
}

static const TestCase g_stand_retire_tests[] = {
    { .name = "stand_retire_program_gone", .func = TestStandRetireProgramGone, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_trash_keeps_the_tags", .func = TestStandRetireTrashKeepsTheTags,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_refusal_resumes", .func = TestStandRetireRefusalResumes, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_ribbon_replay", .func = TestStandRetireRibbonReplay, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_tally_holds", .func = TestStandRetireTallyHolds, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_mount_sweeps", .func = TestStandRetireMountSweeps, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_retire_mount_finishes", .func = TestStandRetireMountFinishes, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_retire_suite = {
    g_stand_retire_tests, sizeof(g_stand_retire_tests) / sizeof(g_stand_retire_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
