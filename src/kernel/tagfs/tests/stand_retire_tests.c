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
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_retire suite: a program left, and its own tags left with it
// (retire/retire.h)
//
// A program's own tags are its labels for itself, and the program is the file
// it was started from. A volume a boot before left holds the tags of the
// programs it ran, which the tests count beside their own. The tests make a "program" of a file, put tags of its
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

static uint32_t StandRetireId(TagFSVolume* vol, uint64_t program, const TagSpelling* tag)
{
    return TagRegistryLookup(vol->state.registry, TagOwnerProgram(program), tag);
}

static bool StandRetireWorn(TagFSVolume* vol, uint64_t file, uint32_t tag)
{
    TagFSMetadata md;
    bool          worn = false;
    if (tagfs_get_metadata(vol, file, &md) != 0)
        return false;
    for (uint32_t i = 0; i < md.tag_count; i++)
        worn = worn || md.tag_ids[i] == tag;
    tagfs_metadata_free(&md);
    return worn;
}

static bool StandRetireWornAnywhere(TagFSVolume* vol, const StandRetire* r, uint32_t tag)
{
    for (uint32_t i = 0; i < 3; i++)
        if (StandRetireWorn(vol, r->files[i], tag))
            return true;
    return false;
}

/* The program, the files, and every tag put on: false when a step would not. */
static bool StandRetireMake(TagFSVolume* vol, StandRetire* r, uint32_t n)
{
    static const char* const names[] = { "retire_a.dat", "retire_b.dat", "retire_c.dat" };
    memset(r, 0, sizeof(*r));
    if (!StandMakeFile(vol, "retire_prog.bin", "prog", 4, &r->program))
        return false;
    for (uint32_t i = 0; i < 3; i++) {
        if (!StandMakeFile(vol, names[i], "data", 4, &r->files[i]))
            return false;
        if (i < n && (tagfs_add_tag_string(vol, TagOwnerProgram(r->program), r->files[i], &s_save) != OK ||
                      tagfs_add_tag_string(vol, TagOwnerProgram(r->program), r->files[i], &s_level) != OK))
            return false;
    }
    if (tagfs_add_tag_string(vol, TagOwnerPerson(), r->files[0], &s_save) != OK)
        return false;
    r->ids[0]  = StandRetireId(vol, r->program, &s_save);
    r->ids[1]  = StandRetireId(vol, r->program, &s_level);
    r->person = TagRegistryLookup(vol->state.registry, TagOwnerPerson(), &s_save);
    return r->ids[0] != TAGFS_INVALID_TAG_ID && r->ids[1] != TAGFS_INVALID_TAG_ID &&
           r->person != TAGFS_INVALID_TAG_ID && r->person != r->ids[0] &&
           tagfs_keep_file(vol, r->files[2], STAND_RETIRE_TERM_S, INK_WARRANTED) == 0;
}

static void StandRetireClean(TagFSVolume* vol, const StandRetire* r)
{
    for (uint32_t i = 0; i < 3; i++)
        (void)tagfs_delete_file(vol, r->files[i], INK_WARRANTED);
    (void)tagfs_delete_file(vol, r->program, INK_WARRANTED);
}

/* What the book says of the program's tags, held to one set of questions
 * wherever the registry came from: both retired, found by no lookup, worn by
 * none of the files, the person's `save` the person's still and worn by
 * the file that wore it, and a program's tag of the same text given another
 * id. */
static bool StandRetireHolds(TagFSVolume* vol, const StandRetire* r)
{
    TagRegistry* reg = vol->state.registry;
    TagSpelling  sp;
    for (uint32_t i = 0; i < 2; i++)
        if (!TagRegistryRetiredId(reg, r->ids[i]) || TagRegistrySpelling(reg, r->ids[i], &sp, NULL, NULL) ||
            StandRetireWornAnywhere(vol, r, r->ids[i]))
            return false;
    return StandRetireId(vol, r->program, &s_save) == TAGFS_INVALID_TAG_ID &&
           StandRetireId(vol, r->program, &s_level) == TAGFS_INVALID_TAG_ID &&
           !TagRegistryRetiredId(reg, r->person) && StandRetireWorn(vol, r->files[0], r->person);
}

/* A new tag of the gone program's text is another id, which the program's
 * absence owes to retirement in its turn. */
static bool StandRetireNeverAgain(TagFSVolume* vol, const StandRetire* r)
{
    uint32_t again = TAGFS_INVALID_TAG_ID;
    if (TagRegistryIntern(vol->state.registry, TagOwnerProgram(r->program), &s_save, &again) != OK)
        return false;
    return again != r->ids[0] && again != r->ids[1] && again > r->ids[1];
}

static TestResult TestStandRetireProgramGone(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    TEST_ASSERT(StandRetireMake(vol, &r, 3), "a program, three files — one kept deleted — and tags of its own on all of them");
    TEST_ASSERT(StandRetireWornAnywhere(vol, &r, r.ids[0]) && StandRetireWorn(vol, r.files[2], r.ids[1]),
                "the program's tags are worn, by the file kept deleted as well");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "a look while the program's file stands");
    TEST_ASSERT_EQ(n, 0, "retires nothing");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, r.program, INK_WARRANTED), 0, "the program's file is deleted with no term");
    TEST_ASSERT(StandRetireWorn(vol, r.files[0], r.ids[0]), "its tags are worn still: nothing was done in the deleter's context");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    TEST_ASSERT_EQ(n, 2, "and retires its two tags");
    TEST_ASSERT(StandRetireHolds(vol, &r), "both are off every file and found by no lookup, and the person's tag of the "
                "same text is the person's still");
    TEST_ASSERT(StandRetireNeverAgain(vol, &r), "a tag of the same text and owner is another id, never the retired one");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the tag the gone program put on since is owed in its turn");
    TEST_ASSERT_EQ(n, 1, "and retired by the next look");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "a look with nothing owed");
    TEST_ASSERT_EQ(n, 0, "retires nothing");
out:
    StandRetireClean(vol, &r);
    return verdict;
}

static TestResult TestStandRetireTrashKeepsTheTags(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    uint64_t    later = KeepNowNs() + (2u * STAND_RETIRE_TERM_S) * 1000000000ull;
    TEST_ASSERT(StandRetireMake(vol, &r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, r.program, STAND_RETIRE_TERM_S, INK_WARRANTED), 0, "the program's file goes to the trash");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    TEST_ASSERT_EQ(n, 0, "and retires nothing: a program in the trash is not gone");
    TEST_ASSERT(StandRetireWornAnywhere(vol, &r, r.ids[0]) && StandRetireWornAnywhere(vol, &r, r.ids[1]) &&
                !TagRegistryRetiredId(vol->state.registry, r.ids[0]), "its tags live");
    TEST_ASSERT_EQ(tagfs_restore_file(vol, r.program, INK_WARRANTED), 0, "the file is brought back");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks again");
    TEST_ASSERT(n == 0 && StandRetireWorn(vol, r.files[1], r.ids[0]) && StandRetireWorn(vol, r.files[2], r.ids[1]) &&
                StandRetireId(vol, r.program, &s_save) == r.ids[0], "restored, it has every tag it had, whole");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, r.program, STAND_RETIRE_TERM_S, INK_WARRANTED), 0, "it goes to the trash again");
    TEST_ASSERT(KeepSweepNow(vol, later) >= 1u, "and its term ends: the volume lets the file go");
    TEST_ASSERT(StandRetireWornAnywhere(vol, &r, r.ids[0]), "the tags are worn until the duty looks");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    TEST_ASSERT_EQ(n, 2, "and retires them");
    TEST_ASSERT(StandRetireHolds(vol, &r), "off every file, the kept one too");
out:
    StandRetireClean(vol, &r);
    return verdict;
}

static TestResult TestStandRetireRefusalResumes(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    StandRetire r;
    uint32_t    n = 0;
    error_t     rc;
    TEST_ASSERT(StandRetireMake(vol, &r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, r.program, INK_WARRANTED), 0, "the program's file is deleted");
    TagFSStandShutForHalt(vol, true);
    rc = RetireSweepNow(vol, &n);
    TagFSStandShutForHalt(vol, false);
    TEST_ASSERT(rc != OK && n == 0, "a volume that takes no change now refuses the duty, which retires nothing");
    TEST_ASSERT(StandRetireWornAnywhere(vol, &r, r.ids[0]), "and takes no tag off a file");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks again once it does");
    TEST_ASSERT_EQ(n, 2, "and finishes what the refusal left: the tags closed, then taken off, then retired");
    TEST_ASSERT(StandRetireHolds(vol, &r), "every tag off every file");
out:
    TagFSStandShutForHalt(vol, false);
    StandRetireClean(vol, &r);
    return verdict;
}

/* The tags of the programs the volume holds besides the test's, none owed:
 * the programs a boot before this one ran keep tags of their own on it. */
static bool StandRetireOthers(TagFSVolume* vol, uint32_t* others)
{
    uint32_t n = 0;
    if (RetireSweepNow(vol, &n) != OK)
        return false;
    *others = vol->state.registry->program_tags;
    return true;
}

/* The state a retirement leaves, read where the volume was mounted afresh. */
static TestResult StandRetireAfterMount(TagFSVolume* vol, StandRetire* r, bool tally)
{
    TestResult  verdict = TEST_PASS;
    static StandBinder b;
    const char* aside = tally ? StandBinderAside(vol, &b) : NULL;
    uint32_t    n = 0, others = 0;
    uint64_t    laid[3] = { 0 }, stood[3] = { 0 };
    if (aside)
        return test_steps_aside(aside);
    memset(r, 0, sizeof(*r));
    TEST_ASSERT(StandRetireOthers(vol, &others), "the other programs' tags counted, none owed");
    TEST_ASSERT(StandRetireMake(vol, r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, r->program, INK_WARRANTED), 0, "the program's file is deleted");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty retires its tags");
    TEST_ASSERT_EQ(n, 2, "both");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle lays the Coineds RETIRED and the files without the tags");
    if (tally) {
        TEST_ASSERT_OK(StandBinderTallies(vol, 1), "and a Tally lays TAGS with the two ids RETIRED");
        EndpaperFactMost(vol, &laid[0], &laid[1], &laid[2]);
    }
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh");
    if (tally) {
        EndpaperFactMost(vol, &stood[0], &stood[1], &stood[2]);
        TEST_ASSERT(laid[2] == stood[2] && laid[0] == stood[0],
                    "the mount measures TAGS and FILES as the Tally's writer did, a retired id one element of TAGS");
    }
    TEST_ASSERT(StandRetireHolds(vol, r), "its book says the same: held as retired, found by no lookup, worn by no file");
    TEST_ASSERT_EQ(vol->state.registry->program_tags, others, "and no program's tag is owed");
    TEST_ASSERT(StandRetireNeverAgain(vol, r), "no id of the book is given twice");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the tag put on since is retired");
    TEST_ASSERT_EQ(n, 1, "by the duty, as before the mount");
out:
    StandRetireClean(vol, r);
    if (tally)
        STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandRetireRibbonReplay(void)
{
    TagFSVolume* vol = TagFSHome();
    StandRetire r;
    return StandRetireAfterMount(vol, &r, false);
}

static TestResult TestStandRetireTallyHolds(void)
{
    TagFSVolume* vol = TagFSHome();
    StandRetire r;
    return StandRetireAfterMount(vol, &r, true);
}

/* The mount owes the retirement of every owner whose file is gone: the cut
 * came before the duty did (`cleared` false), or after the files were cleared
 * and before the tags were retired. */
static TestResult StandRetireAfterCut(TagFSVolume* vol, bool cleared)
{
    TestResult  verdict = TEST_PASS;
    StandRetire r = { 0 };
    uint32_t    n = 0, others = 0;
    TEST_ASSERT(StandRetireOthers(vol, &others), "the other programs' tags counted, none owed");
    TEST_ASSERT(StandRetireMake(vol, &r, 3), "a program, three files and tags of its own on all of them");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, r.program, INK_WARRANTED), 0, "the program's file is deleted");
    for (uint32_t i = 0; cleared && i < 3; i++) {
        InkReceipt receipt;
        TEST_ASSERT_OK(InkTagsOff(vol, r.files[i], r.ids, 2, &receipt), "a file is taken off its tags by hand");
    }
    TEST_ASSERT_OK(TagFSSettle(vol), "the book holds the file gone, the tags in their Coineds");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh: the duty never ran");
    TEST_ASSERT_EQ(vol->state.registry->program_tags, others + 2u,
                   "the registry holds the program's two tags, owed");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty the mount owes");
    TEST_ASSERT_EQ(n, 2, "retires both");
    TEST_ASSERT(StandRetireHolds(vol, &r), "off every file, found by no lookup");
    TEST_ASSERT_OK(TagFSSettle(vol), "the cycle lays them");
    TEST_ASSERT(StandStitchRemount(vol), "and the volume is mounted afresh again");
    TEST_ASSERT(StandRetireHolds(vol, &r) && vol->state.registry->program_tags == others, "retired, none owed");
out:
    StandRetireClean(vol, &r);
    return verdict;
}

static TestResult TestStandRetireMountSweeps(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRetireAfterCut(vol, false);
}

static TestResult TestStandRetireMountFinishes(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRetireAfterCut(vol, true);
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
