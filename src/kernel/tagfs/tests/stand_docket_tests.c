#include "stand_stitch.h"
#include "volume_ribbon.h"
#include "../ink/ink.h"
#include "../mount/book_mount.h"
#include "../tag_registry/tag_registry.h"
#include "../file_map/file_map.h"
#include "../ink/fallow.h"
#include "../block_map/block_map_derived.h"
#include "../use_memory/use_memory.h"
#include "../mount/away.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_docket suite: a book's Ink — files made, named, tagged and given
// up on a format-v2 volume, each change a Jot of the Docket the next stitch
// carries (spec_v2 §2.8, §3.4, §3.6, §3.7)
//
// After every change and the cycle that takes it, the volume mounted again on
// what its medium holds holds the file as memory did before; changes of one
// file between two stitches merge — a file born and given up before a stitch
// named it leaves no record, a Born and the renames after it one Born of the
// last name, two renames of a settled file one Labeled of the last (a file's
// data is stand_fresh's, stand_fresh_tests.c). Ground a file gave up rests
// until the ribbon has left the Quire its stitch lies in — the last of its
// Quires, for one that runs on into the next — and the lag's stitches settled
// past it, two on a medium that lied;
// room asked for closes that Quire early, and what a mount finds the ribbon
// let go rests the same way. A change claims its room on the ribbon first —
// the room past the stitch in flight, the Coineds of every tag given an id
// held with it — waits while room can come and is refused where none can, a
// handle's view being no room a cycle brings; a stitch the kernel sewed, a
// sector of it unreadable, is closed and damaged at the next mount; and a
// medium that leaves and comes back loses the file no stitch settled; the
// warrant guards a book's files as format v1's. Every test puts the ribbon
// back as it found it (stand_stitch.h).
// ============================================================================

/* Whether file `fid`, as memory holds it, is called `name`, wears its name's
 * tag, and wears `tag` — or not, `wears` false. */
static bool StandDocketHolds(uint64_t fid, const char* name, const TagSpelling* tag, bool wears)
{
    TagFSState* fs   = tagfs_get_state();
    FileRecord* rec  = FileMapRecordHold(fs->files, fid, NULL);
    uint32_t    stem = InkNameTagLookup(name), id = TagRegistryLookup(fs->registry, tag);
    bool        has_stem = false, has_tag = false;
    if (!rec)
        return false;
    for (uint32_t i = 0; i < rec->tag_count; i++) {
        has_stem |= rec->tags[i] == stem;
        has_tag  |= rec->tags[i] == id;
    }
    bool holds = strcmp(rec->name, name) == 0 && has_stem && has_tag == wears;
    FileRecordRelease(rec);
    return holds;
}

/* A file the maker laid on the book with blocks of its own that a call with
 * no warrant may give up — no kernel, nothing the system keeps: *fid. */
static bool StandDocketPlainFile(uint64_t* fid)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec   = FileMapRecordHold(fs->files, ids[i], NULL);
            bool        plain = rec && rec->blocks && !InkRecordTouchesBoot(rec) &&
                                !InkWearsGuarded(rec->tags, rec->tag_count);
            FileRecordRelease(rec);
            if (plain) {
                *fid = ids[i];
                return true;
            }
        }
        from = ids[n - 1] + 1;
    }
    return false;
}

/* Whether the map holds no file `fid`. */
static bool StandDocketGone(uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    FileRecordRelease(rec);
    return rec == NULL;
}

/* Whether file `fid` is in the map. */
static bool StandDocketLive(uint64_t fid)
{
    return !StandDocketGone(fid);
}

/* The cycle taken, and the volume mounted again on what its medium holds. */
static bool StandDocketSettledAndBack(void)
{
    return TagFSSettle() == OK && StandStitchRemount();
}

/* The records of `kind` the last stitch this mount sewed carries. */
static uint32_t StandDocketCarries(uint64_t kind)
{
    RibbonUnit* u = NULL;
    uint32_t    n = 0;
    if (!StandStitchRead(&u))
        return UINT32_MAX;
    for (uint32_t i = 0; i < u->nrecords; i++)
        n += u->records[i].kind == kind;
    RibbonUnitLetGo(u);
    return n;
}

/* Whether the last stitch's one Labeled names file `fid` `name`. */
static bool StandDocketLabeledNames(uint64_t fid, const char* name)
{
    RibbonUnit* u    = NULL;
    bool        said = false;
    if (!StandStitchRead(&u))
        return false;
    for (uint32_t i = 0; i < u->nrecords; i++) {
        RibbonLabeled l;
        uint32_t      used = 0;
        if (u->records[i].kind != RIBBON_LABELED ||
            RibbonLabeledGet(u->stream + u->records[i].body, u->records[i].len, &l, &used) == TALLY_BODY_BROKEN)
            continue;
        said = l.fid == fid && (l.what & LABEL_NAME) && l.name_len == strlen(name) &&
               memcmp(l.name, name, l.name_len) == 0;
    }
    RibbonUnitLetGo(u);
    return said;
}

// ----------------------------------------------------------------------------
// Every change, settled and mounted again, is what memory held (§2.8)
// ----------------------------------------------------------------------------

static TestResult TestStandDocketEachChangeMountsAgain(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagSpelling tag;
    uint64_t    fid = 0, plain = 0, given = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(TagSpell("docket:x", 8u, &tag) == OK, "a tag is spelled");
    TEST_ASSERT_EQ(tagfs_create_file("docket-a.txt", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_OK(tagfs_add_tag_string(fid, &tag, INK_UNWARRANTED), "and tagged with a tag new to the volume");
    TEST_ASSERT(StandDocketSettledAndBack(), "its cycle settles, and the volume mounts again");
    TEST_ASSERT(StandDocketHolds(fid, "docket-a.txt", &tag, true), "holding the file, its name's tag and the tag");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "docket-b.txt", INK_UNWARRANTED), 0, "the file is renamed");
    TEST_ASSERT(StandDocketSettledAndBack() && StandDocketHolds(fid, "docket-b.txt", &tag, true),
                "and mounted again it holds the new name and its tag, the tag kept");
    TEST_ASSERT_EQ(tagfs_remove_tag_string(fid, &tag, INK_UNWARRANTED), 0, "the tag is taken off");
    TEST_ASSERT(StandDocketSettledAndBack() && StandDocketHolds(fid, "docket-b.txt", &tag, false),
                "and mounted again the file wears it no more");
    TEST_ASSERT_EQ(tagfs_delete_file(fid, INK_UNWARRANTED), 0, "the file is given up");
    TEST_ASSERT(StandDocketSettledAndBack(), "its cycle settles, and the volume mounts again");
    TEST_ASSERT(StandDocketGone(fid), "without the file");
    TEST_ASSERT(g_book.next_fid > fid, "and the next file id is past it: an id is given once");
    TEST_ASSERT(StandDocketPlainFile(&plain), "a file the maker laid on the book, with blocks");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "is given up, its blocks with it");
    TEST_ASSERT(StandDocketSettledAndBack(), "its cycle settles, and the volume mounts again");
    TEST_ASSERT(StandDocketGone(plain), "without it");
    TEST_ASSERT_EQ(tagfs_create_file("docket-c.txt", NULL, 0, &given), 0, "a file made after the mount");
    TEST_ASSERT(given > fid, "takes an id past every one the book names");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// Changes of a file between two stitches merge (§3.7, L-8)
// ----------------------------------------------------------------------------

static TestResult TestStandDocketMerges(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint64_t    kept = 0, gone = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(tagfs_create_file("docket-first.txt", NULL, 0, &kept), 0, "a file is made");
    TEST_ASSERT_EQ(tagfs_rename_file(kept, "docket-kept.txt", INK_UNWARRANTED), 0, "and renamed before a cycle");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle settles");
    TEST_ASSERT(StandDocketCarries(RIBBON_BORN) == 1 && StandDocketCarries(RIBBON_LABELED) == 0,
                "its stitch carries one Born — of the last name — and no Labeled");
    TEST_ASSERT_EQ(tagfs_create_file("docket-gone.txt", NULL, 0, &gone), 0, "a file is made");
    TEST_ASSERT_EQ(tagfs_delete_file(gone, INK_UNWARRANTED), 0, "and given up before a cycle named it");
    TEST_ASSERT_EQ(tagfs_rename_file(kept, "docket-once.txt", INK_UNWARRANTED), 0, "the settled file is renamed");
    TEST_ASSERT_EQ(tagfs_rename_file(kept, "docket-last.txt", INK_UNWARRANTED), 0, "and renamed again");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle settles");
    TEST_ASSERT(StandDocketCarries(RIBBON_BORN) == 0 && StandDocketCarries(RIBBON_QUITCLAIM) == 0,
                "its stitch carries no record of the file born and given up");
    TEST_ASSERT(StandDocketCarries(RIBBON_LABELED) == 1 && StandDocketLabeledNames(kept, "docket-last.txt"),
                "and one Labeled of the renamed file, of its last name");
    TEST_ASSERT(g_book.next_fid > gone, "the id given is not given again while the volume stays mounted — no "
                "stitch names it, so a mount of what the medium holds may (§4.5 п.8)");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    TEST_ASSERT(StandDocketGone(gone), "without the file given up");
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, kept, NULL);
    TEST_ASSERT(rec && strcmp(rec->name, "docket-last.txt") == 0, "and with the renamed file by its last name");
    FileRecordRelease(rec);
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// Ground given up rests until the ribbon has left the Quire its stitch lies in,
// and room asked for closes that Quire early (§3.4, В1, M-1)
// ----------------------------------------------------------------------------

/* The first block file `fid` names, and how many it names: false for none. */
static bool StandDocketGround(uint64_t fid, uint64_t* first, uint64_t* blocks)
{
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    bool        named = false;
    StripWalk   walk;
    Strip       run;
    if (rec) {
        StripWalkBegin(rec, &walk);
        named   = StripWalkNextGround(&walk, &run);
        *first  = run.start;
        *blocks = rec->blocks;
    }
    FileRecordRelease(rec);
    return named;
}

/* Room asked for, as the allocator's refusal asks it, inside the door. */
static error_t StandDocketRoom(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = SettleForRoom();
    tagfs_leave();
    return rc;
}

/* Whether the ribbon stands in Quire `q`. */
static bool StandDocketStandsIn(const RibbonQuire* q)
{
    RibbonCursor now;
    RibbonCursorStand(&now);
    return RibbonQuireSame(&now.quire, q);
}

static TestResult TestStandDocketGroundRests(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagFSState* fs    = tagfs_get_state();
    uint64_t    plain = 0, first = 0, blocks = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain) && StandDocketGround(plain, &first, &blocks),
                "a file the maker laid on the book names blocks");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "it is given up");
    for (uint64_t k = 0; k <= FallowLag() + 1u; k++)
        TEST_ASSERT_OK(TagFSSettle(), "its cycle, and the lag's after it, settle");
    TEST_ASSERT(StandDocketStandsIn(&s.at.quire), "in the Quire the ribbon stood in");
    TEST_ASSERT(!BlockMapDerivedIsFree(first) && FallowBlocks() >= blocks,
                "its ground rests: the stitch that gave it up lies in a Quire the ribbon has not left");
    RibbonCursor was, now;
    RibbonCursorStand(&was);
    uint64_t free = fs->ledger.free_blocks;
    TEST_ASSERT_OK(StandDocketRoom(), "room is asked for");
    RibbonCursorStand(&now);
    TEST_ASSERT(RibbonQuireSame(&now.quire, &was.next) && now.sewn.off == 0,
                "and the Quire is closed early: the ribbon went on at the start of its next_quire");
    uint64_t taken = now.next.blocks && !RibbonQuireSame(&now.next, &was.next) ? now.next.blocks : 0;
    TEST_ASSERT(BlockMapDerivedIsFree(first) && fs->ledger.free_blocks + taken == free + blocks,
                "the file's ground is free once the lag's stitches settled past it there — all of it, less "
                "the next_quire the ribbon was given in the new Quire");
    TEST_ASSERT_EQ(FallowBlocks(), 0, "and nothing rests");
    TEST_ASSERT(StandStitchRemount() && StandDocketGone(plain), "mounted again, the volume holds no such file");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static TestResult TestStandDocketMountGroundRests(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    MountBookSaid said;
    uint64_t      plain = 0, first = 0, blocks = 0;
    const char*   aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain) && StandDocketGround(plain, &first, &blocks),
                "a file the maker laid on the book names blocks");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "it is given up");
    TEST_ASSERT(StandDocketSettledAndBack(), "its cycle settles, and the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.fallow >= blocks && FallowBlocks() == said.fallow,
                "what the ribbon let go lies fallow, every block of it resting in Fallow (M-1)");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this session settles in the Quire the ribbon stood in");
    TEST_ASSERT(!BlockMapDerivedIsFree(first), "and gives nothing back: the ribbon has not left its Quire");
    TEST_ASSERT_OK(StandDocketRoom(), "room is asked for");
    TEST_ASSERT(BlockMapDerivedIsFree(first) && FallowBlocks() == 0,
                "and the Quire closed early, the lag's stitches of this session settled, the ground is free");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A change claims its room on the ribbon before it is made (P21, §3.6)
// ----------------------------------------------------------------------------

static TestResult TestStandDocketClaimRefused(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagFSState* fs    = tagfs_get_state();
    bool        kept  = false, held = false;
    error_t     rc    = OK;
    uint64_t    fid   = 0, plain = 0, k = 10;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain), "the book holds a file with blocks");
    TEST_ASSERT(BlockMapDerivedStandShrink(EndpaperKeeps(s.at.quire_blocks) + s.at.quire_blocks),
                "its free ground is cut to one Quire past the Tally's");
    held = true;
    for (; rc == OK && k < 10u + 3u * s.at.quire_blocks; k++)
        rc = StandStitchSayLong(16u * TALLY_SHEET_PAYLOAD, (char)('a' + k % 26u), k, &kept);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_NO_SPACE, "words are laid down until the ribbon runs out");
    uint32_t next = fs->registry->next_id;
    TEST_ASSERT_EQ(tagfs_create_file("docket-full.txt", NULL, 0, &fid), -(int)ERR_TAGFS_NO_SPACE,
                   "a file is not made where its Born has no room on the ribbon and none can come");
    TEST_ASSERT_EQ(fs->registry->next_id, next, "and its name's tag is not interned");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "a file is given up all the same");
    TEST_ASSERT(TagFSSettle() == OK && SettledGen() == InkedGen(),
                "and its Quitclaim settles in the rest of the Quire kept for what frees — the word the ribbon "
                "refused was left out of the stitch that refused it, and holds nothing back");
    BlockMapDerivedStandGrow();
    held = false;
    TEST_ASSERT_EQ(tagfs_create_file("docket-full.txt", NULL, 0, &fid), -(int)ERR_WOULD_BLOCK,
                   "with ground back, a create waits for the cycle that gives the ribbon its next_quire");
    TEST_ASSERT_OK(TagFSSettle(), "that cycle settles");
    TEST_ASSERT_EQ(tagfs_create_file("docket-full.txt", NULL, 0, &fid), 0, "and the file is made");
    TEST_ASSERT_OK(TagFSSettle(), "its Born settles");
out:
    if (held)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* The room a claim is judged by lies past the stitch in flight (§16.2 п.8):
 * one that stays in its Quire leaves the rest less itself and the next_quire;
 * one that runs on into the next_quire leaves no room for what does not free,
 * the next_quire's own not given yet. */
static TestResult TestStandDocketRoomPastFlight(void)
{
    TestResult   verdict = TEST_PASS;
    RibbonCursor c;
    uint64_t     any = 0, frees = 0;

    memset(&c, 0, sizeof(c));
    c.up    = true;
    c.atom  = TAGFS_BLOCK_SECTORS;
    c.quire = (RibbonQuire){ 1024, 16 };
    c.next  = (RibbonQuire){ 2048, 16 };
    c.off   = 16u * TAGFS_BLOCK_SECTORS - 2u * c.atom;
    uint64_t quire = 16u * TAGFS_BLOCK_SECTORS;
    RibbonRoomOf(&c, 0, &any, &frees);
    TEST_ASSERT(any == quire && frees == quire,
                "with nothing in flight: two atoms of the rest and the next_quire, less the two atoms kept");
    RibbonRoomOf(&c, c.atom, &any, &frees);
    TEST_ASSERT(any == quire - c.atom && frees == any, "past a stitch in flight that stays in its Quire: an atom less");
    RibbonRoomOf(&c, 3u * c.atom, &any, &frees);
    TEST_ASSERT(any == 0 && frees == quire - c.atom - c.atom,
                "past one that runs on into the next_quire: nothing for what does not free, its next_quire not "
                "given yet; for what frees, the rest of that Quire less the atom kept");
out:
    return verdict;
}

/* A file made whose Born the stitch that runs on into the next_quire carries,
 * with the ground cut short of a next_quire's own: the Born settles, the ribbon
 * stands in that Quire with none past it, a create is refused before anything
 * changes and a file given up settles in the rest kept. */
static TestResult TestStandDocketClaimAcross(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor now;
    TagFSState*  fs    = tagfs_get_state();
    bool         kept  = false, held = false;
    uint64_t     fid   = 0, more = 0, plain = 0;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain), "the book holds a file with blocks");
    TEST_ASSERT(BlockMapDerivedStandShrink(EndpaperKeeps(s.at.quire_blocks) + s.at.quire_blocks - 1u),
                "its free ground is cut short of a Quire past the Tally's: no next_quire past the next is given");
    held = true;
    RibbonCursorStand(&now);
    uint64_t left = now.quire.blocks * TAGFS_BLOCK_SECTORS - now.off;
    TEST_ASSERT(left > 2u * now.atom && now.next.blocks, "the ribbon has more than two atoms of its Quire, and a next");
    TEST_ASSERT_OK(StandStitchSayFilling(left - 2u * now.atom, 'w', 70, &kept), "a word leaves two atoms of it");
    TEST_ASSERT_EQ(tagfs_create_file("across.txt", NULL, 0, &fid), 0, "a file is made: its Born claims its room");
    TEST_ASSERT_OK(StandStitchSayFilling(4u * now.atom, 'x', 71, &kept), "a word four atoms long is said");
    RibbonCursorStand(&now);
    TEST_ASSERT(RibbonQuireSame(&now.quire, &s.at.next) && !now.next.blocks && SettledGen() == InkedGen(),
                "and its stitch, the Born with it, runs on into the next_quire and settles: no next_quire past it");
    uint32_t next = fs->registry->next_id;
    TEST_ASSERT_EQ(tagfs_create_file("across-more.txt", NULL, 0, &more), -(int)ERR_TAGFS_NO_SPACE,
                   "a file is not made where no room can come");
    TEST_ASSERT_EQ(fs->registry->next_id, next, "and its name's tag is not interned");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "a file is given up");
    TEST_ASSERT(TagFSSettle() == OK && SettledGen() == InkedGen(), "and its Quitclaim settles in the rest kept");
    TEST_ASSERT(StandStitchRemount() && StandDocketLive(fid) && StandDocketGone(plain),
                "mounted again: the file made is there, the one given up is not");
out:
    if (held)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* Ground an open handle still views rests whatever the ribbon does — no cycle
 * brings it back while the handle is open: with no next_quire and no room, a
 * create is refused at once, not told to wait, and no stitch is laid for it. */
static TestResult TestStandDocketHandleHolds(void)
{
    TestResult       verdict = TEST_PASS;
    StandStitch      s;
    RibbonCursor     was, now;
    TagFSFileHandle* handle = NULL;
    bool             held   = false;
    uint64_t         plain  = 0, first = 0, blocks = 0, fid = 0;
    const char*      aside  = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain) && StandDocketGround(plain, &first, &blocks),
                "a file the maker laid on the book names blocks");
    handle = tagfs_open(plain, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "it is opened");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "and given up while open");
    TEST_ASSERT_OK(TagFSSettle(), "its Quitclaim settles");
    TEST_ASSERT(BlockMapDerivedStandShrink(EndpaperKeeps(s.at.quire_blocks) + s.at.quire_blocks - 1u),
                "the free ground is cut short of a Quire past the Tally's");
    held = true;
    (void)StandDocketRoom();
    RibbonCursorStand(&was);
    TEST_ASSERT(RibbonQuireSame(&was.quire, &s.at.next) && !was.next.blocks,
                "room asked for closes the Quire early: the ribbon stands in the next with none past it");
    TEST_ASSERT(!BlockMapDerivedIsFree(first) && FallowBlocks() >= blocks,
                "the lag's stitches settled, and the file's ground rests for the open handle");
    TEST_ASSERT_EQ(tagfs_create_file("held.txt", NULL, 0, &fid), -(int)ERR_TAGFS_NO_SPACE,
                   "a file is refused at once: no cycle brings back what a handle views");
    RibbonCursorStand(&now);
    TEST_ASSERT(now.off == was.off && now.last == was.last, "and no stitch was laid for it");
    tagfs_close(handle);
    handle = NULL;
    TEST_ASSERT_OK(TagFSSettle(), "the handle closed, a cycle settles");
    TEST_ASSERT(BlockMapDerivedIsFree(first), "and the ground comes back");
out:
    if (handle)
        tagfs_close(handle);
    if (held)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* ‼ ON A MEDIUM THAT LIED THE LAG IS TWO STITCHES (§3.4 (2), §16.2 п.10):
 * ground the ribbon left rests until the second stitch settled past the one
 * that gave it up — the Quire left by the first is not enough. */
static TestResult TestStandDocketLiedLag(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor was, now;
    uint64_t     plain = 0, first = 0, blocks = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom != VOLUME_BLOCK_SECTORS)
        return test_steps_aside("the medium's atom is larger than a block: a stitch laid by hand here is one");
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandStitchLayEmpty(&s, STITCH_DATA_FLUSHED | STITCH_MEDIUM_LIED) != 0,
                "a stitch saying the medium lied is laid at the ribbon's end");
    TEST_ASSERT(StandRibbonUp() && FallowLag() == 2u, "the volume mounts on it, its lag two stitches");
    TEST_ASSERT(StandDocketPlainFile(&plain) && StandDocketGround(plain, &first, &blocks),
                "a file the maker laid on the book names blocks");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "it is given up");
    TEST_ASSERT_OK(TagFSSettle(), "its Quitclaim settles");
    RibbonCursorStand(&was);
    TEST_ASSERT_OK(StandDocketRoom(), "room is asked for, closing the Quire early");
    RibbonCursorStand(&now);
    TEST_ASSERT(StandDocketStandsIn(&s.at.next) && BlockMapDerivedIsFree(first), "and the ground is free");
    TEST_ASSERT_EQ(now.last - was.last, 2,
                   "given back by the second stitch past its own, not the first that left its Quire: the lag is two");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* ‼ A STITCH THAT RUNS ON INTO ITS NEXT_QUIRE LIES IN THAT ONE (§3.4): what it
 * gave up rests until the ribbon leaves the next_quire, not the Quire it began
 * in — a stitch after it there gives nothing back. */
static TestResult TestStandDocketQuitclaimRunsOn(void)
{
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    RibbonCursor now;
    bool         kept  = false;
    uint64_t     plain = 0, first = 0, blocks = 0;
    const char*  aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketPlainFile(&plain) && StandDocketGround(plain, &first, &blocks),
                "a file the maker laid on the book names blocks");
    RibbonCursorStand(&now);
    uint64_t left = now.quire.blocks * TAGFS_BLOCK_SECTORS - now.off;
    TEST_ASSERT(left > 2u * now.atom && now.next.blocks, "the ribbon has more than two atoms of its Quire, and a next");
    TEST_ASSERT_OK(StandStitchSayFilling(left - 2u * now.atom, 'r', 80, &kept), "a word leaves two atoms of it");
    TEST_ASSERT_EQ(tagfs_delete_file(plain, INK_UNWARRANTED), 0, "the file is given up");
    TEST_ASSERT_OK(StandStitchSayFilling(4u * now.atom, 's', 81, &kept), "and a word four atoms long said");
    RibbonCursorStand(&now);
    TEST_ASSERT(RibbonQuireSame(&now.sewn.quire, &s.at.quire) && RibbonQuireSame(&now.quire, &s.at.next),
                "its stitch, the Quitclaim with it, began in the Quire and runs on into the next");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch settles in the next_quire");
    TEST_ASSERT_OK(TagFSSettle(), "and the lag's after it");
    TEST_ASSERT(!BlockMapDerivedIsFree(first), "the ground rests: the stitch that gave it up lies in the next_quire");
    TEST_ASSERT_OK(StandDocketRoom(), "room is asked for, closing the next_quire early");
    TEST_ASSERT(BlockMapDerivedIsFree(first), "and the ground is free once the ribbon left it");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* Every tag given an id since the last stitch settled holds its Coined's room,
 * whoever interned it: the registry counts it, and the stitch that lays it
 * lets it go (settle/ribbon.c, RibbonHeld). */
static TestResult TestStandDocketCoinedCounted(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagSpelling tag;
    uint32_t    count = 0, id = 0;
    uint64_t    text  = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(TagFSSettle(), "a cycle lays every Coined owed");
    TagRegistryCoinedHeld(tagfs_get_state()->registry, &count, &text);
    TEST_ASSERT(count == 0 && text == 0, "and none is held");
    TEST_ASSERT(TagSpell("docket:counted", 14u, &tag) == OK, "a tag is spelled");
    TEST_ASSERT_OK(TagFSInternSpelling(&tag, &id), "and given an id outside any change of a file");
    TagRegistryCoinedHeld(tagfs_get_state()->registry, &count, &text);
    TEST_ASSERT(count == 1 && text == (uint64_t)tag.key_len + tag.value_len,
                "its Coined is held: one, of its key and value");
    TEST_ASSERT_OK(TagFSSettle(), "the stitch that lays it settles");
    TagRegistryCoinedHeld(tagfs_get_state()->registry, &count, &text);
    TEST_ASSERT(count == 0 && text == 0, "and lets it go");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A stitch the kernel sewed, a sector of it unreadable at the next mount, is
// closed and damaged: what reads by its lead stands (C13, §3.9)
// ----------------------------------------------------------------------------

static TestResult TestStandDocketClosedDamaged(void)
{
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    MountBookSaid said;
    RibbonCursor  one, two;
    bool          kept  = false;
    uint64_t      a = 0, b = 0;
    const char*   aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(tagfs_create_file("c13-a.txt", NULL, 0, &a), 0, "a file is made");
    TEST_ASSERT(StandStitchSayLong(1500u, 'q', 30, &kept) == OK && kept,
                "and a word of 1500 bytes said: one stitch carries the file's Born past the word");
    RibbonCursorStand(&one);
    TEST_ASSERT_EQ(tagfs_create_file("c13-b.txt", NULL, 0, &b), 0, "a second file is made");
    TEST_ASSERT_OK(TagFSSettle(), "and its stitch sewn after the first");
    RibbonCursorStand(&two);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    BlockFailArmBadSectors(s.seat, s.base + RibbonPlaceVlba(&one.sewn) + 1u, 1);
    StandRibbonArrive();
    TagFSStandHoldAgain();
    BlockFailDisarm();
    TEST_ASSERT(tagfs_get_state()->initialized, "the volume mounts with sector 1 of the first stitch unreadable");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_STITCH_TORN], 1, "that stitch, the kernel's own, is said closed and damaged");
    TEST_ASSERT_EQ(said.last, two.last, "and the stitch after it is sewn on");
    TEST_ASSERT(StandDocketLive(a) && StandDocketLive(b),
                "the Born past the lost sector, read by its lead, stands, and so does the file after it");
    UseText* back = tagfs_recall_use_context(NULL);
    TEST_ASSERT(!back || back->bytes != 1500u, "the word that crossed the lost sector is lost");
    UseTextDrop(back);
out:
    BlockFailDisarm();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A book's medium that leaves and comes back is read again: a file no stitch
// settled is lost, and its waiter told (§16.2 п.28)
// ----------------------------------------------------------------------------

static TestResult TestStandDocketReplugLoses(void)
{
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    uint64_t                 fid  = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_EQ(tagfs_create_file("replug-file.txt", NULL, 0, &fid), 0, "a file is made, no stitch carrying it");
    SettleWaitLink(&waiter.w, InkedGen(), StandStitchAnswered);
    TagFSStandMediumLeaves();
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(NULL), AWAY_REREAD, "the medium leaves and comes back: read again");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_DEVICE_NOT_READY,
                "the file's waiter is told it is lost, not answered OK");
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, as a book read again is");
    StandRibbonArrive();
    TagFSStandHoldAgain();
    TEST_ASSERT(StandDocketGone(fid), "mounted again, it holds what its book holds — not the file");
out:
    if (away)
        (void)TagFSStandMediumReturns(NULL);
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The warrant guards a book's files as it guards format v1's (RT-09)
// ----------------------------------------------------------------------------

/* A file the maker laid on the book wearing a tag the system keeps: *fid. */
static bool StandDocketGuardedFile(uint64_t* fid)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec     = FileMapRecordHold(fs->files, ids[i], NULL);
            bool        guarded = rec && InkWearsGuarded(rec->tags, rec->tag_count);
            FileRecordRelease(rec);
            if (guarded) {
                *fid = ids[i];
                return true;
            }
        }
        from = ids[n - 1] + 1;
    }
    return false;
}

static TestResult TestStandDocketWarrant(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    TagSpelling tag;
    uint64_t    kept = 0, fid = 0;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandDocketGuardedFile(&kept), "the book holds a file the system keeps");
    TEST_ASSERT_EQ(tagfs_delete_file(kept, INK_UNWARRANTED), -(int)ERR_PERMISSION_DENIED,
                   "it is not given up by a call with no warrant");
    TEST_ASSERT_EQ(tagfs_create_file("warrant.txt", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT(TagSpell("system:docket", 13u, &tag) == OK, "a tag of key system is spelled");
    TEST_ASSERT_EQ(tagfs_add_tag_string(fid, &tag, INK_UNWARRANTED), ERR_PERMISSION_DENIED,
                   "and is not put on by a call with no warrant");
    TEST_ASSERT(StandDocketSettledAndBack(), "the cycle settles, and the volume mounts again");
    TEST_ASSERT(StandDocketLive(kept), "with the kept file");
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(rec && !InkWearsGuarded(rec->tags, rec->tag_count), "and the new one wearing no tag the system keeps");
    FileRecordRelease(rec);
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_docket_tests[] = {
    { .name = "stand_docket_each_change_mounts_again", .func = TestStandDocketEachChangeMountsAgain,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_merges", .func = TestStandDocketMerges,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_ground_rests", .func = TestStandDocketGroundRests,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_mount_ground_rests", .func = TestStandDocketMountGroundRests,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_claim_refused", .func = TestStandDocketClaimRefused,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_room_past_flight", .func = TestStandDocketRoomPastFlight,
      .reach = TEST_READS_ONLY },
    { .name = "stand_docket_claim_across", .func = TestStandDocketClaimAcross,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_handle_holds", .func = TestStandDocketHandleHolds,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_lied_lag", .func = TestStandDocketLiedLag,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_quitclaim_runs_on", .func = TestStandDocketQuitclaimRunsOn,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_coined_counted", .func = TestStandDocketCoinedCounted,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_closed_damaged", .func = TestStandDocketClosedDamaged,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_replug_loses", .func = TestStandDocketReplugLoses,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_docket_warrant", .func = TestStandDocketWarrant,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_docket_suite = {
    g_stand_docket_tests, sizeof(g_stand_docket_tests) / sizeof(g_stand_docket_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
