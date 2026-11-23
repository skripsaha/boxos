#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../retire/retire.h"
#include "../settle/settle.h"
#include "../tag_registry/tag_registry.h"
#include "storage_tags.h"   /* StorageMaySpeak, StorageViewAll — the deck's one judge */
#include "op_registry.h"
#include "boxos_decks.h"
#include "process.h"
#include "cabin.h"
#include "auth_tags.h"
#include "tagfs_volume.h"

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
// its list with it; a signed program one program in every file of it. And the asks: a note a pair, gone with the answer — yes,
// no, yes to every program —, with either program leaving, and with a mount.
// And the deck's judge, asked as a cabin of the editor asks it: its own tags,
// another program's seen when shown and by the person's eyes, taken off by
// the person's hands, put on by nobody else; the view of every owner it may
// see; and storage.tag.show itself.
// ============================================================================

#define STAND_SHOW_TERM_S  3600ull
#define STAND_SHOW_ROOM    8u

static process_t s_show_proc;
static cabin_t   s_show_cabin;

typedef struct {
    uint64_t editor;
    uint64_t player;
    uint64_t converter;
    uint64_t other;      /* a program shown nothing by name */
} StandShow;

static bool StandShowMake(TagFSVolume* vol, StandShow* s)
{
    memset(s, 0, sizeof(*s));
    return StandMakeFile(vol, "show_editor.box", "edit", 4, &s->editor) &&
           StandMakeFile(vol, "show_player.box", "play", 4, &s->player) &&
           StandMakeFile(vol, "show_convert.box", "conv", 4, &s->converter) &&
           StandMakeFile(vol, "show_other.box", "othr", 4, &s->other);
}

static void StandShowClean(TagFSVolume* vol, const StandShow* s)
{
    (void)tagfs_delete_file(vol, s->editor, INK_WARRANTED);
    (void)tagfs_delete_file(vol, s->player, INK_WARRANTED);
    (void)tagfs_delete_file(vol, s->converter, INK_WARRANTED);
    (void)tagfs_delete_file(vol, s->other, INK_WARRANTED);
}

/* Whether `program`'s card names exactly `want`, `n` of them, and wears the
 * bit exactly when it names any. */
static bool StandShowList(TagFSVolume* vol, uint64_t program, const uint64_t* want, uint32_t n)
{
    uint64_t*     list  = NULL;
    uint32_t      count = 0;
    TagFSMetadata md;
    bool          same  = TagFSShownBy(vol, program, &list, &count) == OK && count == n &&
                          tagfs_get_metadata(vol, program, &md) == 0;
    if (!same) {
        kfree(list);
        return false;
    }
    same = md.shown_count == n && ((md.flags & TAGFS_FILE_SHOWN) != 0) == (n != 0);
    for (uint32_t i = 0; same && i < n; i++)
        same = list[i] == want[i] && md.shown_to[i] == want[i];
    tagfs_metadata_free(&md);
    kfree(list);
    return same;
}

/* Whether the programs that show their tags to `reader` are exactly `want`. */
static bool StandShowShownTo(TagFSVolume* vol, uint64_t reader, const uint64_t* want, uint32_t n)
{
    uint64_t  got[STAND_SHOW_ROOM];
    TagFSPage page;
    if (TagFSShownToPage(vol, reader, 1, got, STAND_SHOW_ROOM, true, &page) != OK || page.delivered != n ||
        page.total != n)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (got[i] != want[i])
            return false;
    return true;
}

/* Whether the programs that asked `owner` and are not answered are exactly
 * `want`. */
static bool StandShowAsked(TagFSVolume* vol, uint64_t owner, const uint64_t* want, uint32_t n)
{
    uint64_t  got[STAND_SHOW_ROOM];
    TagFSPage page;
    if (TagFSAsksPage(vol, owner, 1, got, STAND_SHOW_ROOM, true, &page) != OK || page.delivered != n || page.total != n)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (got[i] != want[i])
            return false;
    return true;
}

static TestResult TestStandShowCard(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandShowList(vol, s.editor, NULL, 0) && !TagFSShows(vol, s.editor, s.player), "a program shows nothing at first");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.converter, true), "the editor shows its tags to the converter");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "and to the player");
    const uint64_t two[] = { s.player, s.converter };
    TEST_ASSERT(StandShowList(vol, s.editor, two, 2), "its card names both, rising, and wears the bit");
    TEST_ASSERT(TagFSShows(vol, s.editor, s.player) && TagFSShows(vol, s.editor, s.converter) &&
                !TagFSShows(vol, s.editor, s.other), "both are shown, the other program is not");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "shown again to the player");
    TEST_ASSERT(StandShowList(vol, s.editor, two, 2), "the card is as it was");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, 0, true), "the editor shows its tags to every program");
    const uint64_t every[] = { 0, s.player, s.converter };
    TEST_ASSERT(StandShowList(vol, s.editor, every, 3) && TagFSShows(vol, s.editor, s.other),
                "every program first on its card, and the other program is shown too");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, 0, false), "every program no longer");
    TEST_ASSERT(StandShowList(vol, s.editor, two, 2) && !TagFSShows(vol, s.editor, s.other),
                "those shown by name still are, the other is not");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.converter, false), "the converter no longer");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.converter, false), "nor again");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, false), "nor the player");
    TEST_ASSERT(StandShowList(vol, s.editor, NULL, 0), "the card names none, and the bit is off");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowRefusals(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT_EQ(TagFSShowTags(vol, s.editor, s.editor, true), ERR_INVALID_ARGUMENT, "a program shown to itself is refused");
    TEST_ASSERT_EQ(TagFSShowTags(vol, 0, s.player, true), ERR_INVALID_ARGUMENT, "and program 0");
    TEST_ASSERT_EQ(TagFSShowTags(vol, s.editor, s.other + 1000000u, true), ERR_FILE_NOT_FOUND,
                   "a reader the volume has not is refused");
    TEST_ASSERT_EQ(TagFSShowTags(vol, s.other + 1000000u, s.player, true), ERR_FILE_NOT_FOUND,
                   "and a program the volume has not");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, s.editor, STAND_SHOW_TERM_S, INK_WARRANTED), 0, "the editor goes to the trash");
    TEST_ASSERT_EQ(TagFSShowTags(vol, s.editor, s.converter, true), ERR_FILE_NOT_FOUND,
                   "a program in the trash shows to nobody new");
    TEST_ASSERT(TagFSShows(vol, s.editor, s.player), "and what it showed stands while it is there");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, false), "one is taken out of its list all the same");
    TEST_ASSERT(StandShowList(vol, s.editor, NULL, 0), "its card names none");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowShownTo(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    uint64_t   got[1];
    TagFSPage  page;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandShowShownTo(vol, s.player, NULL, 0), "nobody shows the player anything at first");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.converter, 0, true), "the converter to every program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.other, s.converter, true), "the other program to the converter alone");
    const uint64_t player[] = { s.editor, s.converter };
    const uint64_t conv[]   = { s.other };
    TEST_ASSERT(StandShowShownTo(vol, s.player, player, 2), "the player is shown by the editor and the converter");
    TEST_ASSERT(StandShowShownTo(vol, s.converter, conv, 1), "the converter by the other program, never by itself");
    TEST_ASSERT_OK(TagFSShownToPage(vol, s.player, 1, got, 1, false, &page), "a page of one for the player");
    TEST_ASSERT(page.delivered == 1 && got[0] == s.editor && page.next == s.converter, "the editor, and the rest after");
    TEST_ASSERT_OK(TagFSShownToPage(vol, s.player, page.next, got, 1, false, &page), "the next page");
    TEST_ASSERT(page.delivered == 1 && got[0] == s.converter && page.next == 0, "the converter, and no more");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowReaderLeft(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    uint32_t   n = 0;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.converter, true), "and to the converter");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, 0, true), "and to every program");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, s.player, STAND_SHOW_TERM_S, INK_WARRANTED), 0, "the player goes to the trash");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    const uint64_t three[] = { 0, s.player, s.converter };
    TEST_ASSERT(StandShowList(vol, s.editor, three, 3), "a reader in the trash has not left: it stays on the card");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, s.player, INK_WARRANTED), 0, "the player is let go for good");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks again");
    const uint64_t two[] = { 0, s.converter };
    TEST_ASSERT(StandShowList(vol, s.editor, two, 2), "the player is off the card, the rest stay");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowProgramLeft(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.converter, s.player, true), "and the converter");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, s.editor, INK_WARRANTED), 0, "the editor is let go");
    const uint64_t conv[] = { s.converter };
    TEST_ASSERT(!TagFSShows(vol, s.editor, s.player) && StandShowShownTo(vol, s.player, conv, 1),
                "its list went with its card: the player is shown by the converter alone");
out:
    StandShowClean(vol, &s);
    return verdict;
}

/* Whether file `fid`'s card names exactly `want`, `n` of them. */
static bool StandShowCardNames(TagFSVolume* vol, uint64_t fid, const uint64_t* want, uint32_t n)
{
    TagFSMetadata md;
    if (tagfs_get_metadata(vol, fid, &md) != 0)
        return false;
    bool same = md.shown_count == n;
    for (uint32_t i = 0; same && i < n; i++)
        same = md.shown_to[i] == want[i];
    tagfs_metadata_free(&md);
    return same;
}

/* Whether file `fid` wears program `owner`'s tag `tag`. */
static bool StandShowWears(TagFSVolume* vol, uint64_t fid, uint64_t owner, const TagSpelling* tag)
{
    uint32_t      id = TagRegistryLookup(vol->state.registry, TagOwnerProgram(owner), tag);
    TagFSMetadata md;
    bool          worn = false;
    if (id == TAGFS_INVALID_TAG_ID || tagfs_get_metadata(vol, fid, &md) != 0)
        return false;
    for (uint32_t i = 0; i < md.tag_count; i++)
        worn = worn || md.tag_ids[i] == id;
    tagfs_metadata_free(&md);
    return worn;
}

/* A signed program is its passport in every file of it (InkPassport): a copy
 * of the editor started is the editor, its card taking the list the first
 * holds; the program is named by its least live file, shows through both,
 * is named once where it shows, and keeps its tags and its list while one
 * copy stays. The passport taken off the last — a program with no signature
 * written over it and started — the card names its file again and holds no
 * list, and the signed program's tags are retired. */
static TestResult TestStandShowSigned(void)
{
    TagFSVolume* vol = TagFSHome();
    static const TagSpelling draft  = { .key = "draft", .key_len = 5 };
    const uint64_t           editor = TAGFS_PASSPORT_BIT | 0xED170Bull;
    TestResult               verdict = TEST_PASS;
    StandShow                s;
    uint64_t                 copy = 0, files[3];
    uint32_t                 n = 0;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandMakeFile(vol, "show_editor2.box", "edit", 4, &copy), "and a second copy of the editor");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "nothing owed before");
    TEST_ASSERT_OK(TagFSPassportLay(vol, s.editor, editor), "the editor is started: its card keeps its passport");
    TEST_ASSERT(TagFSProgramOf(vol, s.editor) == editor && TagFSProgramOf(vol, copy) == copy &&
                TagFSProgramCard(vol, editor) == s.editor && TagFSProgramCard(vol, s.editor) == 0,
                "its file names the program, the copy not started yet is a program of its own");
    TEST_ASSERT_OK(TagFSShowTags(vol, editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(tagfs_add_tag_string(vol, TagOwnerProgram(editor), s.other, &draft), "and wears one on a file");
    TEST_ASSERT_OK(TagFSPassportLay(vol, copy, editor), "the copy is started: its card keeps the same passport");
    TEST_ASSERT(TagFSProgramFiles(vol, editor, files, 3) == 2 && files[0] == s.editor && files[1] == copy,
                "the program is both files");
    const uint64_t player[] = { s.player };
    TEST_ASSERT(StandShowCardNames(vol, copy, player, 1), "the copy's card took the list the first one holds");
    TEST_ASSERT_OK(TagFSShowTags(vol, editor, s.converter, true), "the editor shows its tags to the converter as well");
    const uint64_t both[] = { s.player, s.converter };
    TEST_ASSERT(StandShowCardNames(vol, s.editor, both, 2) && StandShowCardNames(vol, copy, both, 2), "both cards hold it");
    const uint64_t card[] = { s.editor };
    TEST_ASSERT(StandShowShownTo(vol, s.player, card, 1) && TagFSShows(vol, editor, s.converter),
                "the player is shown by the editor once, named by its card");
    TEST_ASSERT_EQ(tagfs_keep_file(vol, s.editor, STAND_SHOW_TERM_S, INK_WARRANTED), 0, "the first copy goes to the trash");
    TEST_ASSERT_EQ(TagFSProgramCard(vol, editor), copy, "the program is named by the live copy");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, s.editor, INK_WARRANTED), 0, "the first copy is let go for good");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    TEST_ASSERT(n == 0 && TagFSShows(vol, editor, s.player) && StandShowWears(vol, s.other, editor, &draft),
                "a copy stays: the tags and the list stay");
    TEST_ASSERT_OK(TagFSPassportLay(vol, copy, 0), "a program with no signature written over the copy is started");
    TEST_ASSERT(TagFSProgramOf(vol, copy) == copy && TagFSProgramFiles(vol, editor, NULL, 0) == 0 &&
                StandShowCardNames(vol, copy, NULL, 0) && !TagFSShows(vol, editor, s.player),
                "the card names its file again, and holds no list: the signed program is gone");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks again");
    TEST_ASSERT(n == 1 && !StandShowWears(vol, s.other, editor, &draft), "and the signed program's tag is retired");
out:
    (void)tagfs_delete_file(vol, copy, INK_WARRANTED);
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowAsks(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    bool       noted = false;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(StandShowAsked(vol, s.editor, NULL, 0), "nobody asked the editor anything");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.editor, &noted) == OK && noted, "the player asks the editor: a note");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.editor, &noted) == OK && !noted, "asked again: the note stands, one");
    TEST_ASSERT(TagFSAskToSee(vol, s.converter, s.editor, &noted) == OK && noted, "the converter asks too");
    const uint64_t both[] = { s.player, s.converter };
    TEST_ASSERT(StandShowAsked(vol, s.editor, both, 2) && StandShowAsked(vol, s.player, NULL, 0),
                "the editor holds both asks, rising, and they are its alone");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor answers the player yes");
    const uint64_t conv[] = { s.converter };
    TEST_ASSERT(StandShowAsked(vol, s.editor, conv, 1) && TagFSShows(vol, s.editor, s.player),
                "the player's note is gone with the answer, and it is shown");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.editor, &noted) == OK && !noted, "one shown already leaves no note asking");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.converter, false), "the editor answers the converter no");
    TEST_ASSERT(StandShowAsked(vol, s.editor, NULL, 0) && !TagFSShows(vol, s.editor, s.converter),
                "its note is gone too, and it is not shown");
    TEST_ASSERT_EQ(TagFSAskToSee(vol, s.editor, s.editor, &noted), ERR_INVALID_ARGUMENT, "a program asking itself is refused");
    TEST_ASSERT_EQ(TagFSAskToSee(vol, s.player, s.other + 1000000u, &noted), ERR_FILE_NOT_FOUND,
                   "and one asking a program the volume has not");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowAsksEvery(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    bool       noted = false;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.editor, &noted) == OK && noted, "the player asks the editor");
    TEST_ASSERT(TagFSAskToSee(vol, s.converter, s.editor, &noted) == OK && noted, "and the converter");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.converter, &noted) == OK && noted, "the player asks the converter too");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, 0, true), "the editor shows its tags to every program");
    const uint64_t player[] = { s.player };
    TEST_ASSERT(StandShowAsked(vol, s.editor, NULL, 0) && StandShowAsked(vol, s.converter, player, 1),
                "every ask of the editor is answered, the converter's ask stands");
    TEST_ASSERT(TagFSAskToSee(vol, s.other, s.editor, &noted) == OK && !noted, "a program shown by every-program asks nothing");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowAsksForgotten(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    bool       noted = false;
    uint32_t   n = 0;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.editor, &noted) == OK && noted, "the player asks the editor");
    TEST_ASSERT(TagFSAskToSee(vol, s.converter, s.editor, &noted) == OK && noted, "and the converter");
    TEST_ASSERT(TagFSAskToSee(vol, s.player, s.other, &noted) == OK && noted, "the player asks the other program");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, s.converter, INK_WARRANTED), 0, "the converter leaves");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, s.other, INK_WARRANTED), 0, "and the other program");
    TEST_ASSERT_OK(RetireSweepNow(vol, &n), "the duty looks");
    const uint64_t player[] = { s.player };
    TEST_ASSERT(StandShowAsked(vol, s.editor, player, 1) && StandShowAsked(vol, s.other, NULL, 0),
                "the notes of a program that left — asking or asked — are gone, the rest stand");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle settles");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh");
    TEST_ASSERT(StandShowAsked(vol, s.editor, NULL, 0), "a mount forgets every note: an ask is asked again");
out:
    StandShowClean(vol, &s);
    return verdict;
}

/* A caller of the deck that is the program `program` of the volume mounted,
 * its box asking `auth`. */
static OpContext StandShowCaller(TagFSVolume* vol, uint64_t program, uint32_t auth)
{
    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    memset(&s_show_proc, 0, sizeof(s_show_proc));
    memset(&s_show_cabin, 0, sizeof(s_show_cabin));
    s_show_cabin.program_fid   = program;
    s_show_cabin.program_owner = program;
    s_show_cabin.auth_bits     = auth;
    memcpy(s_show_cabin.program_volume, vol->state.uuid, sizeof(s_show_cabin.program_volume));
    s_show_proc.cabin = &s_show_cabin;
    ctx.proc          = &s_show_proc;
    return ctx;
}

static TestResult TestStandShowJudge(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    OpContext      ctx    = StandShowCaller(vol, s.editor, AUTH_TAG_APP);
    const TagOwner own    = TagOwnerProgram(s.editor);
    const TagOwner player = TagOwnerProgram(s.player);
    TEST_ASSERT(StorageMaySpeak(&ctx, own, STORAGE_SEE) == OK && StorageMaySpeak(&ctx, own, STORAGE_PUT) == OK &&
                StorageMaySpeak(&ctx, own, STORAGE_TAKE) == OK, "the editor speaks its own tags every way");
    TEST_ASSERT(StorageMaySpeak(&ctx, player, STORAGE_SEE) == ERR_ACCESS_DENIED &&
                StorageMaySpeak(&ctx, player, STORAGE_PUT) == ERR_ACCESS_DENIED &&
                StorageMaySpeak(&ctx, player, STORAGE_TAKE) == ERR_ACCESS_DENIED,
                "the player's tags it neither sees, puts on nor takes off");
    TEST_ASSERT(StorageMaySpeak(&ctx, TagOwnerPerson(), STORAGE_SEE) == ERR_ACCESS_DENIED,
                "nor sees the person's, its box asking nothing");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.player, s.editor, true), "the player shows its tags to the editor");
    TEST_ASSERT(StorageMaySpeak(&ctx, player, STORAGE_SEE) == OK &&
                StorageMaySpeak(&ctx, player, STORAGE_PUT) == ERR_ACCESS_DENIED &&
                StorageMaySpeak(&ctx, player, STORAGE_TAKE) == ERR_ACCESS_DENIED,
                "shown them, the editor sees them, and still puts on and takes off none");
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP | AUTH_TAG_SEE_TAGS);
    TEST_ASSERT(StorageMaySpeak(&ctx, TagOwnerProgram(s.converter), STORAGE_SEE) == OK &&
                StorageMaySpeak(&ctx, TagOwnerPerson(), STORAGE_SEE) == OK &&
                StorageMaySpeak(&ctx, TagOwnerProgram(s.converter), STORAGE_TAKE) == ERR_ACCESS_DENIED &&
                StorageMaySpeak(&ctx, TagOwnerPerson(), STORAGE_PUT) == ERR_ACCESS_DENIED,
                "the person's eyes see every program's tags and the person's, and change none");
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP | AUTH_TAG_CHANGE_TAGS);
    TEST_ASSERT(StorageMaySpeak(&ctx, TagOwnerProgram(s.converter), STORAGE_TAKE) == OK &&
                StorageMaySpeak(&ctx, TagOwnerProgram(s.converter), STORAGE_PUT) == ERR_ACCESS_DENIED &&
                StorageMaySpeak(&ctx, TagOwnerPerson(), STORAGE_PUT) == OK,
                "the person's hands take a program's mark off, put none on, and change the person's");
    ctx = StandShowCaller(vol, 0, AUTH_TAG_APP);
    TEST_ASSERT(StorageMaySpeak(&ctx, TagOwnerProgram(0), STORAGE_SEE) == OK &&
                StorageMaySpeak(&ctx, TagOwnerProgram(0), STORAGE_PUT) == ERR_TAGFS_NO_OWN_TAGS &&
                StorageMaySpeak(&ctx, player, STORAGE_SEE) == ERR_ACCESS_DENIED,
                "a program started from bytes sees its own tags — none — puts none on, and is shown none");
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP);
    s_show_cabin.program_volume[0] ^= 0xFFu;
    TEST_ASSERT(StorageMaySpeak(&ctx, own, STORAGE_PUT) == ERR_TAGFS_NO_OWN_TAGS &&
                StorageMaySpeak(&ctx, player, STORAGE_SEE) == ERR_ACCESS_DENIED,
                "nor one started from another volume, whose file this one's id does not name");
out:
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowViewAll(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    TagView    view;
    uint64_t*  shown = NULL;
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.player, s.editor, true), "the player shows its tags to the editor");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.converter, 0, true), "the converter to every program");
    OpContext ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP);
    TEST_ASSERT_OK(StorageViewAll(&ctx, &view, &shown), "the editor's view of every owner it may see");
    TEST_ASSERT(TagViewLets(&view, TagOwnerProgram(s.editor)) && TagViewLets(&view, TagOwnerProgram(s.player)) &&
                TagViewLets(&view, TagOwnerProgram(s.converter)), "its own, and those shown it by name and to all");
    TEST_ASSERT(!TagViewLets(&view, TagOwnerProgram(s.other)) && !TagViewLets(&view, TagOwnerPerson()),
                "and not the other program's, nor the person's");
    kfree(shown);
    shown = NULL;
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP | AUTH_TAG_SEE_TAGS);
    TEST_ASSERT_OK(StorageViewAll(&ctx, &view, &shown), "the view with the person's eyes");
    TEST_ASSERT(TagViewLets(&view, TagOwnerProgram(s.other)) && TagViewLets(&view, TagOwnerPerson()),
                "every program's and the person's");
out:
    kfree(shown);
    StandShowClean(vol, &s);
    return verdict;
}

static TestResult TestStandShowDeck(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    StandShow  s;
    uint8_t    params[17];
    TagFSPage  page;
    uint64_t   asked[1];
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    OpContext ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP);
    params[0]     = STORAGE_SHOW_TO;
    memcpy(params + 1, &s.player, 8);
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_SHOW, params, 9, NULL, 0, &ctx), "the editor shows the player its tags");
    TEST_ASSERT(TagFSShows(vol, s.editor, s.player), "and the player is shown");
    params[0] = STORAGE_HIDE_FROM;
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_SHOW, params, 9, NULL, 0, &ctx), "and hides them again");
    TEST_ASSERT(!TagFSShows(vol, s.editor, s.player), "and the player is not shown");
    params[0] = 7;
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_SHOW, params, 9, NULL, 0, &ctx), ERR_INVALID_ARGUMENT,
                   "an act the deck does not know is refused");
    ctx       = StandShowCaller(vol, s.player, AUTH_TAG_APP);
    params[0] = STORAGE_ASK_TO_SEE;
    memcpy(params + 1, &s.editor, 8);
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_SHOW, params, 9, NULL, 0, &ctx), "the player asks the editor");
    TEST_ASSERT(TagFSAsksPage(vol, s.editor, 1, asked, 1, false, &page) == OK && page.delivered == 1 &&
                asked[0] == s.player, "and the editor holds its note");
    ctx = StandShowCaller(vol, 0, AUTH_TAG_APP | AUTH_TAG_CHANGE_TAGS);
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_SHOW, params, 9, NULL, 0, &ctx), ERR_TAGFS_NO_OWN_TAGS,
                   "a program started from bytes has no list to show nor a name to ask by, whatever its box asks");

    /* tag.set and tag.unset judge before they read the tag they bring: one
     * the judge lets through is refused for the tag it did not bring. */
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP);
    memcpy(params, &s.converter, 8);
    params[8] = STORAGE_TAGS_PROGRAM;
    memcpy(params + 9, &s.player, 8);
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_SET, params, 17, NULL, 0, &ctx), ERR_ACCESS_DENIED,
                   "the editor puts none of the player's tags on a file");
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, params, 17, NULL, 0, &ctx), ERR_ACCESS_DENIED,
                   "nor takes one off");
    ctx = StandShowCaller(vol, s.editor, AUTH_TAG_APP | AUTH_TAG_CHANGE_TAGS);
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, params, 17, NULL, 0, &ctx), ERR_INVALID_ARGUMENT,
                   "the person's hands take it off: past the judge, refused for the tag they did not name");
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_SET, params, 17, NULL, 0, &ctx), ERR_ACCESS_DENIED,
                   "and put none on");
    params[8] = STORAGE_TAGS_ALL;
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, params, 9, NULL, 0, &ctx), ERR_INVALID_ARGUMENT,
                   "every owner's at once is no owner to take a tag of");
    params[8] = STORAGE_TAGS_PROGRAM;
    memset(params + 9, 0, 8);
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, params, 17, NULL, 0, &ctx), ERR_INVALID_ARGUMENT,
                   "nor program 0");
out:
    StandShowClean(vol, &s);
    return verdict;
}

/* The card through a mount: off the ribbon, then — `tally` — off a Tally. */
static TestResult StandShowAfterMount(TagFSVolume* vol, bool tally)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandShow          s;
    const char*        aside = tally ? StandBinderAside(vol, &b) : NULL;
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandShowMake(vol, &s), "an editor, a player, a converter and another program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, s.player, true), "the editor shows its tags to the player");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.editor, 0, true), "and to every program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.converter, s.other, true), "the converter to the other program");
    TEST_ASSERT_OK(TagFSShowTags(vol, s.converter, s.other, false), "and no longer");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle lays the cards");
    if (tally)
        TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "and a Tally lays their headers");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh");
    const uint64_t editor[] = { 0, s.player };
    TEST_ASSERT(StandShowList(vol, s.editor, editor, 2), "the editor's card is as it was laid");
    TEST_ASSERT(StandShowList(vol, s.converter, NULL, 0), "the converter's shows nothing, its bit off");
    const uint64_t other[] = { s.editor };
    TEST_ASSERT(StandShowShownTo(vol, s.other, other, 1), "the files that show are known to the mount");
out:
    StandShowClean(vol, &s);
    if (tally)
        STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandShowRibbonReplay(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandShowAfterMount(vol, false);
}

static TestResult TestStandShowTallyHolds(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandShowAfterMount(vol, true);
}

static const TestCase g_stand_show_tests[] = {
    { .name = "stand_show_card", .func = TestStandShowCard, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_refusals", .func = TestStandShowRefusals, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_shown_to", .func = TestStandShowShownTo, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_reader_left", .func = TestStandShowReaderLeft, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_program_left", .func = TestStandShowProgramLeft, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_signed", .func = TestStandShowSigned, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_asks", .func = TestStandShowAsks, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_asks_every", .func = TestStandShowAsksEvery, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_asks_forgotten", .func = TestStandShowAsksForgotten, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_judge", .func = TestStandShowJudge, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_view_all", .func = TestStandShowViewAll, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_deck", .func = TestStandShowDeck, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_ribbon_replay", .func = TestStandShowRibbonReplay, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_show_tally_holds", .func = TestStandShowTallyHolds, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_show_suite = {
    g_stand_show_tests, sizeof(g_stand_show_tests) / sizeof(g_stand_show_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
