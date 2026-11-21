#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../ink/ink.h"
#include "../file_map/file_map.h"
#include "../block_map/block_map.h"
#include "write_job.h"   /* WriteJobSubmit, StorageCallWarrant — the deck's road and its warrant */
#include "block_fail.h"  /* BLOCKFAIL that refuses nothing — a count of the writes to a block */
#include "op_registry.h"
#include "boxos_decks.h"
#include "process.h"
#include "cabin.h"
#include "auth_tags.h"   /* the fixed authority bits a cabin's warrant is read from */
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The key with the service cabins — InkWarrant
//
// A file the system keeps is in a role — system, boot or kernel, bits of its
// record's flags — and only a call that carries the warrant changes it: a
// write, a span, a delete. No tag has a say: a file's tags are its user's,
// whatever their keys, and any tag goes on or comes off any file with no
// warrant, the words system, boot and god among them. The storage deck gives a
// program's call the warrant when its cabin holds system or god
// (StorageCallWarrant); a caller in the kernel carries none. The blocks the
// Deed points the loader at are changed by no call, warrant or none, and a file
// the system keeps is never moved to gather its extents (B10). And the deck
// answers an untag with the volume's own reason (Z07-3).
//
// Each test changes only files of its own. The kernel's file is asked one
// write, refused before a byte reaches the medium; every other question about
// the loader's blocks points the Deed's word at a file of the test's own for
// the length of the test — the guard is its only reader in the kernel.
// ============================================================================

static process_t  s_deck_proc;
static cabin_t    s_deck_cabin;
static VolumeBoot s_boot_kept;
static bool       s_boot_moved;

/* A write, a span or a defrag of the file, asked as the deck asks it, with
 * `warrant`, by a caller in the kernel: the job's answer. */
static int StandWarrantJob(WriteJobKind kind, uint64_t file_id, InkWarrant warrant,
                           const void* data, uint32_t bytes, uint64_t size)
{
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind          = kind;
    ask.file_id       = file_id;
    ask.warrant       = warrant;
    ask.source.kernel = data;
    ask.source.bytes  = bytes;
    ask.size          = size;
    return WriteJobSubmit(&ask, NULL, NULL);
}

/* Whether the file wears the tag `text` spells. */
static bool StandWears(uint64_t file_id, const char* text)
{
    uint32_t id = tagfs_tag_lookup(text);
    return id != TAGFS_INVALID_TAG_ID && tagfs_has_tag(file_id, id);
}

/* The tag `text` spells put on the file. */
static error_t StandPutOn(uint64_t file_id, const char* text)
{
    TagSpelling tag;
    error_t     rc = TagSpell(text, strlen(text), &tag);
    return rc == OK ? tagfs_add_tag_string(file_id, &tag) : rc;
}

/* The tag `text` spells taken off the file: 0 or -ERR_*. */
static int StandTakeOff(uint64_t file_id, const char* text)
{
    TagSpelling tag;
    error_t     rc = TagSpell(text, strlen(text), &tag);
    return rc == OK ? tagfs_remove_tag_string(file_id, &tag) : -(int)rc;
}

/* The file's size, how many extents its record names, and where its first one
 * begins. False when the record does not read. */
static bool StandShape(uint64_t file_id, uint64_t* size, uint32_t* extents, uint64_t* first)
{
    FileSpot spot;
    if (!tagfs_enter())
        return false;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    if (rec) {
        *size    = rec->size;
        *extents = (uint32_t)rec->strip_count;
        *first   = FileRecordFind(rec, 0, &spot) ? spot.block : 0;
    }
    bool held = rec != NULL;
    FileRecordRelease(rec);
    tagfs_leave();
    return held;
}

/* The Deed's word on the kernel's blocks pointed at one block of the data run,
 * and put back — a block of the volume in the u32 the BOOT stamp carries. */
static void StandBootOnto(uint64_t block)
{
    TagFSState* fs = tagfs_get_state();
    if (!s_boot_moved)
        s_boot_kept = fs->boot;
    s_boot_moved           = true;
    fs->boot.kernel_block  = (uint32_t)block;
    fs->boot.kernel_blocks = 1;
}

static void StandBootBack(void)
{
    if (s_boot_moved)
        tagfs_get_state()->boot = s_boot_kept;
    s_boot_moved = false;
}

// ----------------------------------------------------------------------------
// The deck answers an untag with the volume's reason (Z07-3)
//
// ObjTagUnset folded every refusal into ERR_INVALID_ARGUMENT: a program could
// not tell a tag its file did not wear from a file that was not there, from a
// medium that left. Asked here as a caller in the kernel asks it — no cabin, so
// no warrant — and the word system, put on a file and taken off, is a tag like
// any other.
// ----------------------------------------------------------------------------

static TestResult TestStandWarrantUntagAnswersItsReason(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;

    TEST_ASSERT(StandMakeFile("stand_warrant_u", NULL, 0, &file_id), "a file of the test's own");
    TEST_ASSERT_OK(StandPutOn(file_id, "system"), "it wears the word system, a tag like any other");
    uint64_t fid  = file_id;                 /* a program's file_id_t */
    uint64_t gone = tagfs_file_ceiling();    /* the next id, as a program names it */
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, &fid, sizeof(fid), "standnone", 9, NULL),
                   ERR_TAGFS_TAG_NOT_FOUND, "unset of a tag the file does not wear says so");
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_TAG_UNSET, &gone, sizeof(gone), "standnone", 9, NULL),
                   ERR_FILE_NOT_FOUND, "and of a file that is not there, that");
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_UNSET, &fid, sizeof(fid), "system", 6, NULL),
                   "and of system, by a caller with no warrant, taken");
    TEST_ASSERT(!StandWears(file_id, "system"), "and the file wears it no more");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write and a span of a file the system keeps: refused without the warrant,
// taken with it
// ----------------------------------------------------------------------------

static TestResult TestStandWarrantWriteAndSpan(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       file_id = 0;
    uint64_t       size    = 0, first = 0;
    uint32_t       extents = 0;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 2 * B, 0x3A770100u, 0);
    TEST_ASSERT(StandMakeFile("stand_warrant_w", g_test_buffer, 2 * B, &file_id),
                "a two-block file of the test's own");
    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_SYSTEM), "it is kept in the system's role");

    TestFillAt(g_test_output, 2 * B, 0x3A770200u, 0);
    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_WRITE, file_id, INK_UNWARRANTED, g_test_output, B, 0),
                   ERR_PERMISSION_DENIED, "a write with no warrant is refused");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output + 2 * B, 2 * B), (int)(2 * B),
                   "the file reads back");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output + 2 * B, g_test_buffer, 2 * B), 2 * B,
                   "as it was");

    TEST_ASSERT_OK(StandWarrantJob(WRITE_JOB_WRITE, file_id, INK_WARRANTED, g_test_output, B, 0),
                   "a write with the warrant is taken");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output + 2 * B, B), (int)B, "the file reads back");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output + 2 * B, g_test_output, B), B,
                   "with the warranted write's bytes in it");

    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_SPAN, file_id, INK_UNWARRANTED, NULL, 0, B),
                   ERR_PERMISSION_DENIED, "a span with no warrant is refused");
    TEST_ASSERT(StandShape(file_id, &size, &extents, &first) && size == 2 * B,
                "and the file keeps its size");
    TEST_ASSERT_OK(StandWarrantJob(WRITE_JOB_SPAN, file_id, INK_WARRANTED, NULL, 0, B),
                   "a span with the warrant is taken");
    TEST_ASSERT(StandShape(file_id, &size, &extents, &first) && size == B,
                "and the file is the size it asked");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A delete of a file the system keeps: by its role, and no tag
//
// The file is named system.standw — a name is a field of its card and wears no
// tag, the bare key system no more than another — and wears system:standwarrant:
// words of its user's, which keep nothing. A rename and an untag of the key
// are taken with no warrant, and a delete of the file is taken. Kept in the
// system's role, the same file is refused its delete without the warrant, and
// an untag or a rename changes nothing of that.
// ----------------------------------------------------------------------------

static TestResult TestStandWarrantDeleteUntagRename(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;

    TEST_ASSERT(StandMakeFile("system.standw", NULL, 0, &file_id), "a file named system.standw");
    TEST_ASSERT(!StandWears(file_id, "system"), "its name is no tag: it wears none of the key system");
    TEST_ASSERT_OK(StandPutOn(file_id, "system:standwarrant"), "and it wears system:standwarrant");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0,
                   "a delete with no warrant is taken: the words keep nothing");
    file_id = 0;

    TEST_ASSERT(StandMakeFile("system.standw", NULL, 0, &file_id), "a file named system.standw again");
    TEST_ASSERT_OK(StandPutOn(file_id, "system:standwarrant"), "wearing system:standwarrant");
    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_SYSTEM), "kept in the system's role");
    TEST_ASSERT_EQ(tagfs_rename_file(file_id, "standwarrant.r"), 0, "a rename with no warrant is taken");
    TEST_ASSERT(!StandWears(file_id, "system") && !StandWears(file_id, "standwarrant"),
                "a rename moves no tag: the file wears neither name's key");
    TEST_ASSERT_EQ(StandTakeOff(file_id, "system"), 0, "an untag of the key system is taken");
    TEST_ASSERT(!StandWears(file_id, "system:standwarrant"), "and the file wears no tag of it");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), -(int)ERR_PERMISSION_DENIED,
                   "a delete with no warrant is refused: the role keeps it the system's");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_WARRANTED), 0, "a delete with the warrant is taken");
    file_id = 0;
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// The blocks the Deed points the loader at: refused to every call
//
// The kernel's own file is asked one warranted write, armed to count the
// writes to its first block: refused before one reaches it. Then the Deed's
// word is pointed at a file of the test's own, and its write, span and delete
// are refused with the warrant and without, and a gather of a Plot loaded
// before is refused too (B10).
// ----------------------------------------------------------------------------

static TestResult StandWarrantBootAsked(uint64_t file_id, InkWarrant warrant)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_WRITE, file_id, warrant, g_test_output,
                                   TAGFS_BLOCK_SIZE, 0),
                   ERR_PERMISSION_DENIED, "a write over the loader's blocks is refused");
    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_SPAN, file_id, warrant, NULL, 0, 0),
                   ERR_PERMISSION_DENIED, "and a span");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, warrant), -(int)ERR_PERMISSION_DENIED,
                   "and a delete");
out:
    return verdict;
}

static TestResult TestStandWarrantBootRunRefusedToBoth(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    kernel  = 0, file_id = 0;
    uint64_t    size    = 0, block = 0, first = 0;
    uint32_t    extents = 0;
    bool        armed   = false, loaded = false;
    Plot        plot;
    TagSpelling key     = TagSpellingOfKey("kernel");
    TagFSPage   page;
    TagFSState* fs      = tagfs_get_state();
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(fs->boot.kernel_blocks > 0, "the Deed names the blocks the loader reads");
    TEST_ASSERT_OK(TagFSNamedPage("kernel", 6, NULL, 0, NULL, 0, 1, &kernel, 1, false, &page),
                   "the kernel is asked for by its name");
    TEST_ASSERT_EQ(page.delivered, 1, "and found");
    TEST_ASSERT(StandBlockOf(kernel, 0, &block) &&
                block == fs->boot.kernel_block,
                "its first block is the one the Deed names");
    TEST_ASSERT_EQ(TestFileRead(kernel, g_test_output, TAGFS_BLOCK_SIZE), (int)TAGFS_BLOCK_SIZE,
                   "the bytes it begins with are read");
    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    int answer = StandWarrantJob(WRITE_JOB_WRITE, kernel, INK_WARRANTED, g_test_output,
                                 TAGFS_BLOCK_SIZE, 0);
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_EQ(answer, ERR_PERMISSION_DENIED, "a warranted write into the kernel is refused");
    TEST_ASSERT_EQ(seen, 0, "before a byte of it reached the kernel's block");

    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x3A770300u, 0);
    TEST_ASSERT(StandMakeFile("stand_warrant_b", g_test_output, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file of the test's own");
    TEST_ASSERT(StandShape(file_id, &size, &extents, &first) && extents == 1,
                "it lies in one run");
    TEST_ASSERT_OK(PlotLoad(&plot, file_id, INK_WARRANTED), "its Plot is loaded with the warrant");
    loaded = true;

    StandBootOnto(first);
    TEST_ASSERT_EQ(PlotGather(&plot, 0), ERR_PERMISSION_DENIED,
                   "with the Deed pointed at it, its blocks are not gathered");
    PlotRelease(&plot);
    loaded = false;
    if (StandWarrantBootAsked(file_id, INK_UNWARRANTED) != TEST_PASS ||
        StandWarrantBootAsked(file_id, INK_WARRANTED) != TEST_PASS) {
        verdict = TEST_FAIL;
        goto out;
    }
    StandBootBack();
    TEST_ASSERT(StandShape(file_id, &size, &extents, &first) && size == TAGFS_BLOCK_SIZE,
                "the file is as it was");
out:
    if (armed)
        BlockFailDisarm();
    if (loaded)
        PlotRelease(&plot);
    StandBootBack();
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// The guard's refusal asks the heap nothing
//
// A delete is guarded on what the file's record holds — the tags it wears and
// the blocks its Strips lie on — read in place, so the refusal of a file the
// system keeps to a call with no warrant, and of the kernel's file to any
// call, comes before the first allocation of the delete: a heap with nothing
// left never turns either into ERR_NO_MEMORY. HEAPFAIL armed on the first
// allocation of the call never fires.
// ----------------------------------------------------------------------------

/* The file given up with `warrant`, the first allocation of the call refused:
 * the answer, and whether any allocation was asked. */
static error_t StandWarrantQuitclaimBare(uint64_t file_id, InkWarrant warrant, bool* asked)
{
    InkReceipt receipt;
    HeapFailArm(amp_get_core_index(), 1);
    error_t rc = InkQuitclaim(file_id, warrant, &receipt);
    *asked = HeapFailFired();
    HeapFailDisarm();
    return rc;
}

static TestResult TestStandWarrantRefusedBeforeMemory(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    file_id = 0;
    uint64_t    size    = 0, first = 0;
    uint32_t    extents = 0;
    bool        asked   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x3A770500u, 0);
    TEST_ASSERT(StandMakeFile("stand_warrant_m", g_test_output, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file of the test's own");
    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_SYSTEM), "it is kept in the system's role");
    TEST_ASSERT_EQ(StandWarrantQuitclaimBare(file_id, INK_UNWARRANTED, &asked),
                   ERR_PERMISSION_DENIED, "its delete with no warrant, the heap refusing, is "
                   "refused for what it wears");
    TEST_ASSERT(!asked, "before an allocation was asked");

    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_ORDINARY), "the role is let go");
    TEST_ASSERT(StandShape(file_id, &size, &extents, &first) && extents == 1,
                "the file lies in one run");
    StandBootOnto(first);
    TEST_ASSERT_EQ(StandWarrantQuitclaimBare(file_id, INK_WARRANTED, &asked), ERR_PERMISSION_DENIED,
                   "with the Deed pointed at it, its delete with the warrant, the heap refusing, is "
                   "refused for the blocks it lies on");
    TEST_ASSERT(!asked, "before an allocation was asked");
    StandBootBack();
out:
    HeapFailDisarm();
    StandBootBack();
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A file the system keeps is not gathered, warrant or none (B10)
//
// The file is given two extents for certain: after its first block, the block
// that follows it is taken by the test, so the file's second block lies past
// it. A defrag with no warrant is refused by the guard; one with the warrant
// passes the guard and is refused by the gather, the extents as they were.
// With the role let go, the same defrag gathers the file into one run — so the
// refusal was the file's being the system's, and nothing else.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// The deck reads the warrant off the cabin
//
// The warrant is the cabin's authority: system or god, and nothing else. And
// the deck's delete, asked by a program whose cabin holds an app's and a
// utility's authority, is refused on a file the system keeps; asked by one
// whose cabin holds system, it is taken.
// ----------------------------------------------------------------------------

static TestResult TestStandWarrantDeckReadsTheCabin(void)
{
    static const struct {
        uint32_t   auth;
        InkWarrant warrant;
    } rows[] = {
        { 0,                                   INK_UNWARRANTED },
        { AUTH_TAG_APP,                        INK_UNWARRANTED },
        { AUTH_TAG_UTILITY | AUTH_TAG_APP,     INK_UNWARRANTED },
        { AUTH_TAG_BYPASS | AUTH_TAG_NETWORK,  INK_UNWARRANTED },
        { AUTH_TAG_SYSTEM,                     INK_WARRANTED   },
        { AUTH_TAG_GOD,                        INK_WARRANTED   },
        { AUTH_TAG_SYSTEM | AUTH_TAG_UTILITY,  INK_WARRANTED   },
    };
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0, kernel_id = 0;
    TagFSPage  page;
    OpContext  ctx;

    memset(&ctx, 0, sizeof(ctx));
    memset(&s_deck_proc, 0, sizeof(s_deck_proc));
    memset(&s_deck_cabin, 0, sizeof(s_deck_cabin));
    TEST_ASSERT_EQ(StorageCallWarrant(NULL), INK_UNWARRANTED, "a call with no context carries none");
    TEST_ASSERT_EQ(StorageCallWarrant(&ctx), INK_UNWARRANTED, "nor one with no process");
    ctx.proc = &s_deck_proc;
    TEST_ASSERT_EQ(StorageCallWarrant(&ctx), INK_UNWARRANTED, "nor one whose process has no cabin");
    s_deck_proc.cabin = &s_deck_cabin;
    for (uint32_t i = 0; i < ARRAY_SIZE(rows); i++) {
        s_deck_cabin.auth_bits = rows[i].auth;
        TEST_ASSERT_EQ(StorageCallWarrant(&ctx), rows[i].warrant,
                       "a cabin's authority gives the warrant exactly when it holds system or god");
    }

    TEST_ASSERT(StandMakeFile("stand_warrant_d", NULL, 0, &file_id), "a file of the test's own");
    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_SYSTEM), "it is kept in the system's role");
    uint64_t fid = file_id;    /* a program's file_id_t */
    s_deck_cabin.auth_bits = AUTH_TAG_UTILITY | AUTH_TAG_APP;
    TEST_ASSERT_EQ(StandDeckAsk(STORAGE_OBJ_DELETE, &fid, sizeof(fid), NULL, 0, &ctx),
                   ERR_PERMISSION_DENIED, "the deck's delete for a utility's cabin is refused");
    TEST_ASSERT_OK(TagFSNamedPage("stand_warrant_d", 15, NULL, 0, NULL, 0, 1, &kernel_id, 1, false, &page),
                   "the file is asked for by its name");
    TEST_ASSERT_EQ(kernel_id, file_id, "and it is still there");
    s_deck_cabin.auth_bits = AUTH_TAG_SYSTEM;
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_OBJ_DELETE, &fid, sizeof(fid), NULL, 0, &ctx),
                   "and for a cabin that holds system, taken");
    file_id = 0;
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// Any tag goes on any file, with no warrant
//
// The words system, boot and god, with a value or none, are tags a user may put
// on any file, through the volume's road and the deck's, and a rename to
// system.standt is a rename: none of them makes the file one the system
// keeps, and the file stays its owner's — it is written with no warrant. Only
// the role of its record keeps a file, and the role is put on by the system
// alone (StandKeepInRole stands for the image builder): then the same write is
// refused, and an ordinary tag still goes on with no warrant.
// ----------------------------------------------------------------------------

static TestResult TestStandWarrantAnyTagPutOn(void)
{
    static const char* const words[] = { "system", "boot:standwarrant", "god:standwarrant", "system:standwarrant" };
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;

    TEST_ASSERT(StandMakeFile("stand_warrant_t", NULL, 0, &file_id), "a file of the test's own");
    uint64_t fid = file_id;    /* a program's file_id_t */
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_SET, &fid, sizeof(fid), "system:standdeck", 16, NULL),
                   "the deck's tag set of system:standdeck, by a caller with no warrant, is taken");
    TEST_ASSERT(StandWears(file_id, "system:standdeck"), "and the file wears it");
    for (uint32_t i = 0; i < ARRAY_SIZE(words); i++) {
        TEST_ASSERT_OK(StandPutOn(file_id, words[i]), "a tag of key system, boot or god goes on with no warrant");
        TEST_ASSERT(StandWears(file_id, words[i]), "and the file wears it");
    }
    TEST_ASSERT_EQ(tagfs_rename_file(file_id, "system.standt"), 0,
                   "a rename to system.standt, with no warrant, is taken too");
    TEST_ASSERT(StandWears(file_id, "system"), "and the file wears the tags it wore: a name is no tag");
    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_WRITE, file_id, INK_UNWARRANTED, g_test_output,
                                   TAGFS_BLOCK_SIZE, 0),
                   OK, "the file is still its owner's: a write with no warrant is taken");

    TEST_ASSERT_OK(StandKeepInRole(file_id, TAGFS_FILE_ROLE_SYSTEM), "kept in the system's role");
    TEST_ASSERT_EQ(StandWarrantJob(WRITE_JOB_WRITE, file_id, INK_UNWARRANTED, g_test_output,
                                   TAGFS_BLOCK_SIZE, 0),
                   ERR_PERMISSION_DENIED, "a write with no warrant is refused: the role keeps it");
    TEST_ASSERT_OK(StandPutOn(file_id, "standnote"),
                   "an ordinary tag goes on a file the system keeps with no warrant");
    TEST_ASSERT_OK(StandDeckAsk(STORAGE_TAG_SET, &fid, sizeof(fid), "standmark", 9, NULL),
                   "and through the deck, by a caller with no warrant");
    TEST_ASSERT(StandWears(file_id, "standnote") && StandWears(file_id, "standmark"),
                "the file wears both");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_WARRANTED);
    return verdict;
}

static const TestCase g_stand_warrant_tests[] = {
    { .name  = "stand_warrant_untag_answers_its_reason",
      .func  = TestStandWarrantUntagAnswersItsReason, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_write_and_span", .func = TestStandWarrantWriteAndSpan,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_delete_untag_rename", .func = TestStandWarrantDeleteUntagRename,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_boot_run_refused_to_both",
      .func  = TestStandWarrantBootRunRefusedToBoth, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_refused_before_memory",
      .func  = TestStandWarrantRefusedBeforeMemory, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_deck_reads_the_cabin", .func = TestStandWarrantDeckReadsTheCabin,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_warrant_any_tag_put_on", .func = TestStandWarrantAnyTagPutOn,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_warrant_suite = {
    .tests = g_stand_warrant_tests, .count = ARRAY_SIZE(g_stand_warrant_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
