#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../retire/retire.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_show suite: a program's own tags shown to other programs
// (show/show.c, tagfs.h TagFSShowTags)
//
// Whom a program shows its tags to is the card of its own file. The tests make
// "programs" of files — an editor that shows, a player and a converter it
// shows to — and hold the list: rising, every program first, changed only by
// what is asked, refused where it cannot be; the same through a mount off the
// ribbon and off a Tally; the programs a reader is shown by, a page at a time;
// a reader that leaves taken out of the lists, a program that leaves taking
// its list with it.
// ============================================================================

#define STAND_SHOW_TERM_S  3600ull
#define STAND_SHOW_ROOM    8u

typedef struct {
    uint64_t editor;
    uint64_t player;
    uint64_t converter;
    uint64_t other;      /* a program shown nothing by name */
} StandShow;

static bool StandShowMake(StandShow* s)
{
    memset(s, 0, sizeof(*s));
    return StandMakeFile("show_editor.box", "edit", 4, &s->editor) &&
           StandMakeFile("show_player.box", "play", 4, &s->player) &&
           StandMakeFile("show_convert.box", "conv", 4, &s->converter) &&
           StandMakeFile("show_other.box", "othr", 4, &s->other);
}

static void StandShowClean(const StandShow* s)
{
    (void)tagfs_delete_file(s->editor, INK_WARRANTED);
    (void)tagfs_delete_file(s->player, INK_WARRANTED);
    (void)tagfs_delete_file(s->converter, INK_WARRANTED);
    (void)tagfs_delete_file(s->other, INK_WARRANTED);
}

/* Whether `program`'s card names exactly `want`, `n` of them, and wears the
 * bit exactly when it names any. */
static bool StandShowList(uint64_t program, const uint64_t* want, uint32_t n)
{
    uint64_t      list[STAND_SHOW_ROOM];
    uint32_t      count = 0;
    TagFSMetadata md;
    if (TagFSShownBy(program, list, STAND_SHOW_ROOM, &count) != OK || count != n || tagfs_get_metadata(program, &md))
        return false;
    bool same = md.shown_count == n && ((md.flags & TAGFS_FILE_SHOWN) != 0) == (n != 0);
    for (uint32_t i = 0; same && i < n; i++)
        same = list[i] == want[i] && md.shown_to[i] == want[i];
    tagfs_metadata_free(&md);
    return same;
}

/* Whether the programs that show their tags to `reader` are exactly `want`. */
static bool StandShowShownTo(uint64_t reader, const uint64_t* want, uint32_t n)
{
    uint64_t  got[STAND_SHOW_ROOM];
    TagFSPage page;
    if (TagFSShownToPage(reader, 1, got, STAND_SHOW_ROOM, true, &page) != OK || page.delivered != n ||
        page.total != n)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (got[i] != want[i])
            return false;
    return true;
}

static TestResult TestStandShowCard(void)
{
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandShowList(s.editor, NULL, 0) && !TagFSShows(s.editor, s.player), "a program shows nothing at first");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.converter, true), "the editor shows its tags to the converter");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "and to the player");
    const uint64_t two[] = { s.player, s.converter };
    TEST_ASSERT(StandShowList(s.editor, two, 2), "its card names both, rising, and wears the bit");
    TEST_ASSERT(TagFSShows(s.editor, s.player) && TagFSShows(s.editor, s.converter) &&
                !TagFSShows(s.editor, s.other), "both are shown, the other program is not");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "shown again to the player");
    TEST_ASSERT(StandShowList(s.editor, two, 2), "the card is as it was");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, 0, true), "the editor shows its tags to every program");
    const uint64_t every[] = { 0, s.player, s.converter };
    TEST_ASSERT(StandShowList(s.editor, every, 3) && TagFSShows(s.editor, s.other),
                "every program first on its card, and the other program is shown too");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, 0, false), "every program no longer");
    TEST_ASSERT(StandShowList(s.editor, two, 2) && !TagFSShows(s.editor, s.other),
                "those shown by name still are, the other is not");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.converter, false), "the converter no longer");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.converter, false), "nor again");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, false), "nor the player");
    TEST_ASSERT(StandShowList(s.editor, NULL, 0), "the card names none, and the bit is off");
out:
    StandShowClean(&s);
    return verdict;
}

static TestResult TestStandShowRefusals(void)
{
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT_EQ(TagFSShowTags(s.editor, s.editor, true), ERR_INVALID_ARGUMENT, "a program shown to itself is refused");
    TEST_ASSERT_EQ(TagFSShowTags(0, s.player, true), ERR_INVALID_ARGUMENT, "and program 0");
    TEST_ASSERT_EQ(TagFSShowTags(s.editor, s.other + 1000000u, true), ERR_FILE_NOT_FOUND,
                   "a reader the volume has not is refused");
    TEST_ASSERT_EQ(TagFSShowTags(s.other + 1000000u, s.player, true), ERR_FILE_NOT_FOUND,
                   "and a program the volume has not");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_EQ(tagfs_keep_file(s.editor, STAND_SHOW_TERM_S, INK_WARRANTED), 0, "the editor goes to the trash");
    TEST_ASSERT_EQ(TagFSShowTags(s.editor, s.converter, true), ERR_FILE_NOT_FOUND,
                   "a program in the trash shows to nobody new");
    TEST_ASSERT(TagFSShows(s.editor, s.player), "and what it showed stands while it is there");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, false), "one is taken out of its list all the same");
    TEST_ASSERT(StandShowList(s.editor, NULL, 0), "its card names none");
out:
    StandShowClean(&s);
    return verdict;
}

static TestResult TestStandShowShownTo(void)
{
    TestResult verdict = TEST_PASS;
    StandShow  s;
    uint64_t   got[1];
    TagFSPage  page;
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandShowShownTo(s.player, NULL, 0), "nobody shows the player anything at first");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(s.converter, 0, true), "the converter to every program");
    TEST_ASSERT_OK(TagFSShowTags(s.other, s.converter, true), "the other program to the converter alone");
    const uint64_t player[] = { s.editor, s.converter };
    const uint64_t conv[]   = { s.other };
    TEST_ASSERT(StandShowShownTo(s.player, player, 2), "the player is shown by the editor and the converter");
    TEST_ASSERT(StandShowShownTo(s.converter, conv, 1), "the converter by the other program, never by itself");
    TEST_ASSERT_OK(TagFSShownToPage(s.player, 1, got, 1, false, &page), "a page of one for the player");
    TEST_ASSERT(page.delivered == 1 && got[0] == s.editor && page.next == s.converter, "the editor, and the rest after");
    TEST_ASSERT_OK(TagFSShownToPage(s.player, page.next, got, 1, false, &page), "the next page");
    TEST_ASSERT(page.delivered == 1 && got[0] == s.converter && page.next == 0, "the converter, and no more");
out:
    StandShowClean(&s);
    return verdict;
}

static TestResult TestStandShowReaderLeft(void)
{
    TestResult verdict = TEST_PASS;
    StandShow  s;
    uint32_t   n = 0;
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.converter, true), "and to the converter");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, 0, true), "and to every program");
    TEST_ASSERT_EQ(tagfs_keep_file(s.player, STAND_SHOW_TERM_S, INK_WARRANTED), 0, "the player goes to the trash");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks");
    const uint64_t three[] = { 0, s.player, s.converter };
    TEST_ASSERT(StandShowList(s.editor, three, 3), "a reader in the trash has not left: it stays on the card");
    TEST_ASSERT_EQ(tagfs_delete_file(s.player, INK_WARRANTED), 0, "the player is let go for good");
    TEST_ASSERT_OK(RetireSweepNow(&n), "the duty looks again");
    const uint64_t two[] = { 0, s.converter };
    TEST_ASSERT(StandShowList(s.editor, two, 2), "the player is off the card, the rest stay");
out:
    StandShowClean(&s);
    return verdict;
}

static TestResult TestStandShowProgramLeft(void)
{
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(s.converter, s.player, true), "and the converter");
    TEST_ASSERT_EQ(tagfs_delete_file(s.editor, INK_WARRANTED), 0, "the editor is let go");
    const uint64_t conv[] = { s.converter };
    TEST_ASSERT(!TagFSShows(s.editor, s.player) && StandShowShownTo(s.player, conv, 1),
                "its list went with its card: the player is shown by the converter alone");
out:
    StandShowClean(&s);
    return verdict;
}

/* The card through a mount: off the ribbon, then — `tally` — off a Tally. */
static TestResult StandShowAfterMount(bool tally)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandShow          s;
    const char*        aside = tally ? StandBinderAside(&b) : NULL;
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandShowMake(&s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(s.editor, 0, true), "and to every program");
    TEST_ASSERT_OK(TagFSShowTags(s.converter, s.other, true), "the converter to the other program");
    TEST_ASSERT_OK(TagFSShowTags(s.converter, s.other, false), "and no longer");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle lays the cards");
    if (tally)
        TEST_ASSERT_OK(StandBinderTallies(1u), "and a Tally lays their headers");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh");
    const uint64_t editor[] = { 0, s.player };
    TEST_ASSERT(StandShowList(s.editor, editor, 2), "the editor's card is as it was laid");
    TEST_ASSERT(StandShowList(s.converter, NULL, 0), "the converter's shows nothing, its bit off");
    const uint64_t other[] = { s.editor };
    TEST_ASSERT(StandShowShownTo(s.other, other, 1), "the files that show are known to the mount");
out:
    StandShowClean(&s);
    if (tally)
        STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandShowRibbonReplay(void)
{
    return StandShowAfterMount(false);
}

static TestResult TestStandShowTallyHolds(void)
{
    return StandShowAfterMount(true);
}

static const TestCase g_stand_show_tests[] = {
    { .name = "stand_show_card", .func = TestStandShowCard, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_refusals", .func = TestStandShowRefusals, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_shown_to", .func = TestStandShowShownTo, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_reader_left", .func = TestStandShowReaderLeft, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_program_left", .func = TestStandShowProgramLeft, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_ribbon_replay", .func = TestStandShowRibbonReplay, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_tally_holds", .func = TestStandShowTallyHolds, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_show_suite = {
    g_stand_show_tests, sizeof(g_stand_show_tests) / sizeof(g_stand_show_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
