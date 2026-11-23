#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../keep/keep.h"
#include "../file_map/file_map.h"
#include "../volume/carry.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_carry suite: a file still arriving (volume/carry.h)
//
// A carry lays its copy on the volume it goes to still arriving: born with
// the name, tags and times of the file it copies, it is met by no question,
// no name and no open but the carry's own, and its writes keep the times it
// was born with; the carry's last Ink takes the bit off — kept deleted, when
// the file lay deleted — and from then on it is the file it was. A copy the
// bit is still on when no carry is under way — the machine stopped in the
// middle of one — is let go by the volume's service, and a mount after
// finds it so. One volume is enough: these are the volume's own rules; the
// carry between two is the twovolumes gate's.
// ============================================================================

#define STAND_CARRY_PAGE  16u

static const char s_stem[] = "carryfile";
static const char s_name[] = "carryfile.dat";

/* The times a copy is born with: another machine's, long ago. */
#define STAND_CARRY_CREATED   1700000000000000000ull
#define STAND_CARRY_MODIFIED  1700000005000000000ull

/* A file born arriving, named s_name and wearing the person's tag s_stem. */
static error_t StandCarryBorn(TagFSVolume* vol, uint64_t* fid)
{
    TagSpelling stem = { .key = s_stem, .key_len = sizeof(s_stem) - 1u };
    uint32_t    tid  = 0;
    error_t     rc   = TagRegistryIntern(vol->state.registry, TagOwnerPerson(), &stem, &tid);
    if (rc != OK)
        return rc;
    InkBirth   b = { .name = s_name, .tags = &tid, .count = 1, .created_ns = STAND_CARRY_CREATED,
                     .modified_ns = STAND_CARRY_MODIFIED };
    InkReceipt receipt;
    return InkNewArriving(vol, &b, fid, &receipt);
}

/* Whether a question meets `fid`: by its tag (the deleted ones' for `kept`),
 * by its name, and in the list of every file. */
static bool StandCarryAmong(TagFSVolume* vol, uint64_t fid, bool kept)
{
    TagSpelling stem = { .key = s_stem, .key_len = sizeof(s_stem) - 1u };
    uint64_t    ids[STAND_CARRY_PAGE];
    TagFSPage   page;
    error_t     rc = kept ? TagFSQueryKeptPage(vol, TagOwnerPerson(), &stem, 1, NULL, 0, 1, ids, STAND_CARRY_PAGE, false, &page)
                          : TagFSQueryPage(vol, TagOwnerPerson(), &stem, 1, NULL, 0, 1, ids, STAND_CARRY_PAGE, false, &page);
    for (uint32_t i = 0; rc == OK && i < page.delivered; i++)
        if (ids[i] == fid)
            return true;
    return false;
}

static bool StandCarryNamed(TagFSVolume* vol, uint64_t fid)
{
    uint64_t  ids[STAND_CARRY_PAGE];
    TagFSPage page;
    error_t   rc = TagFSNamedPage(vol, TagOwnerPerson(), s_name, sizeof(s_name) - 1u, NULL, 0, NULL, 0, 1, ids,
                                  STAND_CARRY_PAGE, false, &page);
    for (uint32_t i = 0; rc == OK && i < page.delivered; i++)
        if (ids[i] == fid)
            return true;
    return false;
}

static bool StandCarryListed(TagFSVolume* vol, uint64_t fid)
{
    uint64_t  ids[STAND_CARRY_PAGE];
    uint64_t  from = 1;
    TagFSPage page;
    while (TagFSListPage(vol, from, ids, STAND_CARRY_PAGE, false, &page) == OK) {
        for (uint32_t i = 0; i < page.delivered; i++)
            if (ids[i] == fid)
                return true;
        if (!page.next)
            break;
        from = page.next;
    }
    return false;
}

/* Whether the volume's map holds `fid`, and the flags of its record then. */
static bool StandCarryHeld(TagFSVolume* vol, uint64_t fid, uint32_t* flags)
{
    FileRecord* rec = FileMapRecordHold(vol->state.files, fid, NULL);
    if (rec && flags)
        *flags = rec->flags;
    FileRecordRelease(rec);
    return rec != NULL;
}

/* The service's sweep asked now, past the stand's hold of it. */
static void StandCarrySweep(TagFSVolume* vol)
{
    __atomic_store_n(&vol->carry.sweep_owed, 1u, __ATOMIC_RELEASE);
    bool held = CarryHold(vol, false);
    CarryServe(vol);
    (void)CarryHold(vol, held);
}

static TestResult TestStandCarryArrivingUnseen(void)
{
    TagFSVolume*     vol     = TagFSHome();
    TestResult       verdict = TEST_PASS;
    uint64_t         fid     = 0, written = 0;
    TagFSFileHandle* h       = NULL;
    TagFSMetadata    md;
    static const char bytes[] = "carried";
    TEST_ASSERT_OK(StandCarryBorn(vol, &fid), "a file is born arriving");
    TEST_ASSERT(!StandCarryAmong(vol, fid, false) && !StandCarryAmong(vol, fid, true),
                "no question by its tag meets it, the deleted ones' neither");
    TEST_ASSERT(!StandCarryNamed(vol, fid), "nor one by its name");
    TEST_ASSERT(!StandCarryListed(vol, fid), "nor the list of every file");
    h = tagfs_open(vol, fid, TAGFS_HANDLE_READ);
    TEST_ASSERT(h == NULL, "an open for anybody but the carry is refused");
    TEST_ASSERT(tagfs_get_metadata(vol, fid, &md) != 0, "and nobody is told its card");
    h = tagfs_open(vol, fid, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_ARRIVAL);
    TEST_ASSERT(h != NULL, "the carry's own open reaches it");
    PlotSource src = { .kernel = (const uint8_t*)bytes, .bytes = sizeof(bytes) - 1u };
    TEST_ASSERT_OK(tagfs_write(vol, h, &src, &written), "its bytes are written");
    tagfs_close(vol, h);
    h = NULL;
    InkReceipt receipt;
    TEST_ASSERT_OK(InkArrived(vol, fid, 0, &receipt), "the file arrives");
    TEST_ASSERT(StandCarryAmong(vol, fid, false) && StandCarryNamed(vol, fid) && StandCarryListed(vol, fid),
                "and every question meets it");
    TEST_ASSERT_EQ(tagfs_get_metadata(vol, fid, &md), 0, "its card is told");
    bool times = md.created_time == STAND_CARRY_CREATED && md.modified_time == STAND_CARRY_MODIFIED;
    bool whole = md.size == sizeof(bytes) - 1u && !(md.flags & TAGFS_FILE_ARRIVING);
    tagfs_metadata_free(&md);
    TEST_ASSERT(times, "with the times it was born with: its write moved neither");
    TEST_ASSERT(whole, "its bytes, and no bit of arriving");
out:
    tagfs_close(vol, h);
    if (fid)
        (void)tagfs_delete_file(vol, fid, INK_UNWARRANTED);
    return verdict;
}

static TestResult TestStandCarryArrivesKept(void)
{
    TagFSVolume* vol     = TagFSHome();
    TestResult   verdict = TEST_PASS;
    uint64_t     fid     = 0, until = KeepUntil(3600u);
    uint32_t     flags   = 0;
    InkReceipt   receipt;
    TEST_ASSERT_OK(StandCarryBorn(vol, &fid), "a file is born arriving");
    TEST_ASSERT_OK(InkArrived(vol, fid, until, &receipt), "it arrives kept deleted, as the file it copies lay");
    TEST_ASSERT(!StandCarryAmong(vol, fid, false) && StandCarryAmong(vol, fid, true),
                "the deleted ones' question meets it, and no other");
    TEST_ASSERT(StandCarryHeld(vol, fid, &flags) && (flags & TAGFS_FILE_DELETED) && !(flags & TAGFS_FILE_ARRIVING),
                "its record is deleted and no longer arriving");
    TEST_ASSERT_OK(InkArrived(vol, fid, until, &receipt), "arriving again changes nothing");
out:
    if (fid)
        (void)tagfs_delete_file(vol, fid, INK_WARRANTED);
    return verdict;
}

static TestResult TestStandCarryCutShortLetGo(void)
{
    TagFSVolume* vol     = TagFSHome();
    TestResult   verdict = TEST_PASS;
    StandStitch  s;
    uint64_t     cut = 0, arrived = 0;
    uint32_t     flags = 0;
    InkReceipt   receipt;
    const char*  aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(StandCarryBorn(vol, &cut), "a copy is born arriving");
    TEST_ASSERT_OK(StandCarryBorn(vol, &arrived), "and another");
    TEST_ASSERT_OK(InkArrived(vol, arrived, 0, &receipt), "which arrives");
    TEST_ASSERT_OK(TagFSSettle(vol), "both are settled: the ribbon lays them");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted afresh, as after a machine stopped mid-carry");
    TEST_ASSERT(StandCarryHeld(vol, cut, &flags) && (flags & TAGFS_FILE_ARRIVING),
                "the copy cut short is read back still arriving");
    TEST_ASSERT(!StandCarryAmong(vol, cut, false) && !StandCarryListed(vol, cut), "and no question meets it");
    __atomic_add_fetch(&vol->carry.inbound, 1u, __ATOMIC_ACQ_REL);
    StandCarrySweep(vol);
    __atomic_sub_fetch(&vol->carry.inbound, 1u, __ATOMIC_ACQ_REL);
    TEST_ASSERT(StandCarryHeld(vol, cut, NULL), "while a carry lays a file on the volume, the service lets none go");
    StandCarrySweep(vol);
    TEST_ASSERT(!StandCarryHeld(vol, cut, NULL), "with none under way, the service lets the copy go");
    TEST_ASSERT(StandCarryHeld(vol, arrived, NULL), "and leaves the one that arrived");
    TEST_ASSERT_OK(TagFSSettle(vol), "the Quitclaim is settled");
    TEST_ASSERT(StandStitchRemount(vol), "and the volume mounted afresh again");
    TEST_ASSERT(!StandCarryHeld(vol, cut, NULL) && StandCarryHeld(vol, arrived, NULL),
                "the copy cut short stays gone; the one that arrived stays");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_carry_tests[] = {
    { .name = "stand_carry_arriving_unseen", .func = TestStandCarryArrivingUnseen,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_carry_arrives_kept", .func = TestStandCarryArrivesKept,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_carry_cut_short_let_go", .func = TestStandCarryCutShortLetGo,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_carry_suite = {
    g_stand_carry_tests, sizeof(g_stand_carry_tests) / sizeof(g_stand_carry_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
