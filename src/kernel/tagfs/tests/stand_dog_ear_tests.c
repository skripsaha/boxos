#include "stand_binder.h"
#include "stand_fresh.h"
#include "../ink/fallow.h"
#include "../integrity/gloss.h"
#include "../settle/binder.h"
#include "../settle/dog_ear.h"
#include "../settle/foliation.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../file_map/file_map.h"
#include "../use_memory/use_memory.h"
#include "crypto.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_dog_ear suite: the keys a Tally's trees changed since the Tally
// before it, and the self-check that holds them to the state (spec_v2
// §16.5 п.9, §4.3 TALLY_TAKE, §2.5)
//
// DogEar is every key of FILES, TAGS, USE and DIGESTS changed since the Tally
// the next one is laid against — by a stitch that settled, by the ribbon a
// mount replayed past its slot, by a copy of a node that would not read — and
// Foliation that Tally's nodes as memory knows them. A Tally written whole
// holds each element of its own whose key DogEar does not hold to the element
// the Tally before it holds under that key, byte for byte (STRICT and the
// stand): an element changed outside DogEar is a DEFECT, and no Tally is
// written over a DogEar the state gives the lie to. The volume's medium does
// not change: every Tally is written whole. The self-check is held to bite on
// every way an element can be changed outside DogEar — a header, a Parcel's
// runs, a Parcel gone, a Parcel new, a digest — each by a lever of the
// stand's, pulled behind every Ink on a key DogEar does not hold — the
// Tallies a Glean asks for laid first where it holds it — and let go (not on
// a STRICT kernel, which stops there). DogEar is held to what each source
// puts in — a stitch of this mount, a mount's replay, a stitch's deletes, a
// copy of a FILES leaf or of a DIGESTS leaf found wanting — and to the Tally
// that clears it. The tests look at the Foliation and the reads' DIGESTS
// table, the Settle's holder's, from outside its Scribe: the runner holds the
// cadence (tests.c, SettleCadenceHold), so no Tally runs but the one a test
// asks for and waits out, and none turns them under its look. Every test puts
// the Spine and the ribbon back as it found them.
// ============================================================================

/* Files of a long name, enough for FILES to take two leaves and more. */
#define STAND_DOG_EAR_MANY  160u

/* The state memory holds: its live files' print and count, the Use Context's
 * print — taken as a mount takes them, from what memory holds now and not
 * from g_book, whose numbers are the last mount's: so the state the stitches
 * left is held to the one a mount of them makes. */
typedef struct {
    uint32_t files_print;
    uint64_t files;
    uint32_t use_print;
} StandDogEarState;

static void StandDogEarStateOf(StandDogEarState* st)
{
    UseText* use    = tagfs_recall_use_context(NULL);
    st->files_print = BookFilesStandPrint();
    st->files       = tagfs_get_state()->ledger.total_files;
    st->use_print   = use && use->bytes ? KCrc32c((const uint8_t*)use->text, (uint32_t)use->bytes) : 0;
    UseTextDrop(use);
}

static bool StandDogEarSame(const StandDogEarState* a, const StandDogEarState* b)
{
    bool same = a->files_print == b->files_print && a->files == b->files && a->use_print == b->use_print;
    if (!same)
        kprintf("[TESTS] stand_dog_ear: the states differ — files print %08X/%08X, files %llu/%llu, Use %08X/%08X\n",
                a->files_print, b->files_print, (unsigned long long)a->files, (unsigned long long)b->files,
                a->use_print, b->use_print);
    return same;
}

/* Whether DogEar holds `key` of `tree` among the spans put in since the last
 * slice — every one, a page of them at a time. */
static bool StandDogEarHolds(uint64_t tree, TallyKey key)
{
    static DogEarSpan spans[256];
    uint32_t          n = 0;
    for (uint32_t from = 0; (n = DogEarHeldSpans(tree, from, spans, 256u)) > 0; from += n)
        for (uint32_t i = 0; i < n; i++)
            if ((key.k1 > spans[i].lo.k1 || (key.k1 == spans[i].lo.k1 && key.k2 >= spans[i].lo.k2)) &&
                (key.k1 < spans[i].hi.k1 || (key.k1 == spans[i].hi.k1 && key.k2 < spans[i].hi.k2)))
                return true;
    return false;
}

/* `key` of `tree`, the one a lever changes, lying outside DogEar — where the
 * self-check holds an element to the Tally before; one whose key DogEar took
 * it leaves to memory (binder_check.c, CheckInLeaf). A Tally's DONE may glean
 * a Tally-Quire, the keys of its leaves put in DogEar for the next Tally to
 * lay (binder_glean.c, GleanNode): on a used book that Quire may hold the
 * lever's leaf, so the Tallies that lay it are written first — a slot of the
 * ring at the most, for a Quire laid again goes into the pair, which Glean
 * never chooses. TEST_PASS, or why not said at the caller's `line` after its
 * `what`: a Tally refused (its rc), DogEar lost, or the key held still. */
static TestResult StandDogEarOutside(int line, uint64_t tree, TallyKey key, const char* what)
{
    char     why[200];
    error_t  rc = OK;
    uint32_t t  = 0;
    for (; rc == OK && StandDogEarHolds(tree, key) && t < g_book.frame.spine_slots; t++)
        rc = StandBinderTallies(1u);
    if (rc != OK) {
        ksnprintf(why, sizeof(why), "%s: a Tally laid to take it out of DogEar was refused (rc, OK)", what);
        return TestFailedAt(__FILE__, line, why, rc, OK);
    }
    if (DogEarStandLost(false)) {
        ksnprintf(why, sizeof(why), "%s: DogEar lost what was put in, so no key is known outside it (lost, 0)",
                  what);
        return TestFailedAt(__FILE__, line, why, 1, 0);
    }
    if (StandDogEarHolds(tree, key)) {
        ksnprintf(why, sizeof(why), "%s: DogEar holds it still past a Tally a slot of the ring (Tallies, slots)",
                  what);
        return TestFailedAt(__FILE__, line, why, t, g_book.frame.spine_slots);
    }
    return TEST_PASS;
}

/* The lever's key outside DogEar (StandDogEarOutside), or the test fails at
 * the line that asked. */
#define STAND_DOG_EAR_OUTSIDE(tree, key, what)                                         \
    do {                                                                               \
        if ((verdict = StandDogEarOutside(__LINE__, (tree), (key), (what))) != TEST_PASS) \
            goto out;                                                                  \
    } while (0)

/* `n` files of a long name made from `from` on, settled: the first id in
 * *first. */
static bool StandDogEarMake(uint32_t from, uint32_t n, uint64_t* first)
{
    char     name[64];
    uint64_t fid = 0;
    for (uint32_t i = 0; i < n; i++) {
        ksnprintf(name, sizeof(name), "dogear-%05u-of-a-name-long-enough.txt", from + i);
        if (tagfs_create_file(name, NULL, 0, &fid) != 0)
            return false;
        if (i == 0)
            *first = fid;
    }
    return TagFSSettle() == OK;
}

// ----------------------------------------------------------------------------
// The self-check bites: an element changed outside DogEar refuses the Tally
// ----------------------------------------------------------------------------

/* The lines the stand says round each lever on the self-check: the book's
 * judge (tools/logcheck_book.sh, stand_book_defects) holds that each pair
 * holds the one DEFECT of the self-check its lever draws, and that no line of
 * TagFS says DEFECT outside them. */
#define STAND_DOG_EAR_LEVER   "[TESTS] stand_dog_ear: a lever pulled behind every Ink"
#define STAND_DOG_EAR_LET_GO  "[TESTS] stand_dog_ear: the lever let go"

/* What a lever changes behind every Ink — no stitch carried it, so no key of
 * it is in DogEar — and what it held before. */
typedef struct {
    FileRecord*  rec;        /* a live file's record */
    const Tract* tract;      /* the runs swapped into it: its own taken, another's given */
    uint64_t     strips;     /* ... and their count of Strips and of blocks */
    uint64_t     blocks;
    Strip*       strip;      /* a run whose birth is moved */
    uint64_t     block;      /* a block whose digest the Gloss is told */
    uint64_t     was;        /* what was changed held before: a time, a birth, a digest */
} StandDogEarHand;

typedef void (*StandDogEarPull)(StandDogEarHand* h, bool on);

/* The lever pulled, a Tally asked for, the lever let go — between the stand's
 * two lines — and at once a stitch and a Tally of the state put back, before
 * the test says a word of what came: a kernel whose Tally the lever did not
 * stop leaves no Tally of a state that never was past the test. What the
 * levered Tally answered; *after what the one after it did. */
static error_t StandDogEarLevered(const char* what, StandDogEarPull pull, StandDogEarHand* h, error_t* after)
{
    kprintf(STAND_DOG_EAR_LEVER ": %s\n", what);
    pull(h, true);
    error_t rc = BinderRunWhole();
    pull(h, false);
    kprintf(STAND_DOG_EAR_LET_GO "\n");
    *after = TagFSSettle();
    if (*after == OK)
        *after = BinderRunWhole();
    return rc;
}

/* A header no stitch carried: the record's time moved (FILES, part 0). */
static void StandDogEarTimeMoved(StandDogEarHand* h, bool on)
{
    if (on) {
        h->was                = h->rec->modified_time;
        h->rec->modified_time = h->was + 1000000000ull;
    } else {
        h->rec->modified_time = h->was;
    }
}

/* A Parcel no stitch carried: a run's birth moved, so the Parcel the Tally
 * before holds, spelled again from this Tally's seq, is not this one's. Below
 * the birth it had — or past it, from the first seq — it stays below every
 * seq that lays it. */
static void StandDogEarBirthMoved(StandDogEarHand* h, bool on)
{
    if (on) {
        h->was          = h->strip->birth;
        h->strip->birth = h->was > 1u ? h->was - 1u : h->was + 1u;
    } else {
        h->strip->birth = h->was;
    }
}

/* Parcels no stitch carried: the record's runs swapped with the hand's — the
 * file's own taken away (a Parcel of the Tally before this one has not), or
 * another file's given to a file of none (one the Tally before has not).
 * Pulled and let go alike: the swap undoes itself. */
static void StandDogEarRunsSwapped(StandDogEarHand* h, bool on)
{
    const Tract* tract  = h->rec->tract;
    uint64_t     strips = h->rec->strip_count, blocks = h->rec->blocks;
    (void)on;
    h->rec->tract       = h->tract;
    h->rec->strip_count = h->strips;
    h->rec->blocks      = h->blocks;
    h->tract            = tract;
    h->strips           = strips;
    h->blocks           = blocks;
}

/* A digest no stitch carried: the Gloss told another for a block — under the
 * state lock, as Ink tells it (ink_book.c) — then the one it had. */
static void StandDogEarGlossTold(StandDogEarHand* h, bool on)
{
    Strip     one = { .start = h->block, .count = 1, .kind = STRIP_DATA, .birth = 0 };
    GlossRoom room;
    if (GlossRoomFor(&one, 1, &room) != OK) {
        kprintf("[TESTS] stand_dog_ear: no memory to tell the Gloss block %llu's digest\n",
                (unsigned long long)h->block);
        return;
    }
    TagFSLockTake(&tagfs_get_state()->lock);
    GlossPut(&room, h->block, on ? ~h->was : h->was);
    TagFSLockDrop(&tagfs_get_state()->lock);
    GlossRoomLetGo(&room);
}

static TestResult TestStandDogEarStrictBites(void)
{
#if CONFIG_STRICT
    return test_steps_aside("a STRICT kernel stops at the self-check's DEFECT: an element changed outside "
                            "DogEar is one it panics on, never one it writes past");
#else
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandDogEarHand    h;
    BinderStandSaid    said;
    uint64_t           fid   = 0;
    error_t            rc    = OK, after = OK;
    const char*        aside = StandBinderAside(&b);

    memset(&h, 0, sizeof(h));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file("dogear-strict.txt", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it is written: every element it changed lies in DogEar");
    BinderStand(&said);
    TestEvidence("elements the Tally's self-check held to the Tally before", said.checked);
    TEST_ASSERT(said.checked > 0, "and its self-check held the rest to the Tally before, element by element");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch settles past it");
    h.rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(h.rec != NULL, "the file's record is held");
    STAND_DOG_EAR_OUTSIDE(TALLY_FILES, ((TallyKey){ fid, 0 }), "its header's key lies outside DogEar");
    rc = StandDogEarLevered("a file's header no stitch carried", StandDogEarTimeMoved, &h, &after);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_CORRUPTED, "the Tally is refused: its self-check finds the file's header changed "
                   "outside DogEar (a DEFECT)");
    TEST_ASSERT_OK(after, "the record as the stitches left it again, a stitch settles and the Tally is written");
out:
    FileRecordRelease(h.rec);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
#endif
}

/* A file of three blocks of data — and, with `e`, an empty one made beside it
 * — settled, a Tally of them written and the volume mounted on it, its runs
 * born where that Tally says: their records held. */
static bool StandDogEarTallied(StandFreshModel* m, const char* name, uint64_t* data, FileRecord** d, FileRecord** e)
{
    uint32_t bytes = 3u * TAGFS_BLOCK_SIZE;
    uint64_t empty = 0;
    char     none[64];
    ksnprintf(none, sizeof(none), "%s.none", name);
    if (tagfs_create_file(name, NULL, 0, data) != 0 || (e && tagfs_create_file(none, NULL, 0, &empty) != 0) ||
        !StandFreshModelMake(m, bytes) || StandFreshWrite(m, *data, 0, bytes, 61) != (int)bytes ||
        TagFSSettle() != OK || BinderRunWhole() != OK || !StandStitchRemount() || TagFSSettle() != OK)
        return false;
    *d = FileMapRecordHold(tagfs_get_state()->files, *data, NULL);
    if (e)
        *e = FileMapRecordHold(tagfs_get_state()->files, empty, NULL);
    return *d && (*d)->tract && (!e || (*e && !(*e)->tract));
}

static TestResult TestStandDogEarRunsBite(void)
{
#if CONFIG_STRICT
    return test_steps_aside("a STRICT kernel stops at the self-check's DEFECT: a Parcel changed outside DogEar is "
                            "one it panics on, never one it writes past");
#else
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    StandDogEarHand    h;
    StripWalk          walk;
    FileRecord*        d = NULL;
    FileRecord*        e = NULL;
    const Strip*       strips = NULL;
    uint32_t           n = 0;
    uint64_t           data = 0;
    error_t            rc = OK, after = OK;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    memset(&h, 0, sizeof(h));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandDogEarTallied(&m, "dogear-runs.bin", &data, &d, &e),
                "a file of data and an empty one, a Tally of them, the volume mounted on it");
    StripWalkBegin(d, &walk);
    TEST_ASSERT(StripWalkParcel(&walk, &strips, &n) && n > 0 && strips[0].kind == STRIP_DATA && strips[0].birth,
                "the data's first run, born where the Tally says");
    h.strip = (Strip*)(uintptr_t)&strips[0];    /* the lever writes what memory holds as never changed */
    STAND_DOG_EAR_OUTSIDE(TALLY_FILES, ((TallyKey){ data, 1 }), "the data's Parcel's key lies outside DogEar");
    rc = StandDogEarLevered("a run's birth no stitch carried", StandDogEarBirthMoved, &h, &after);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_CORRUPTED, "refused: the Parcel, spelled again from this Tally's seq, differs");
    TEST_ASSERT_OK(after, "the birth put back, the Tally is written");
    h = (StandDogEarHand){ .rec = d, .tract = NULL, .strips = 0, .blocks = 0 };
    STAND_DOG_EAR_OUTSIDE(TALLY_FILES, ((TallyKey){ data, 1 }), "and lies outside it again");
    rc = StandDogEarLevered("a file's runs taken away by no stitch", StandDogEarRunsSwapped, &h, &after);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_CORRUPTED, "refused: a Parcel of the Tally before is not in this one");
    TEST_ASSERT_OK(after, "the runs put back, the Tally is written");
    h = (StandDogEarHand){ .rec = e, .tract = d->tract, .strips = d->strip_count, .blocks = d->blocks };
    STAND_DOG_EAR_OUTSIDE(TALLY_FILES, ((TallyKey){ e->file_id, 1 }),
                          "the empty file's first Parcel's key lies outside DogEar");
    rc = StandDogEarLevered("another file's runs given an empty one by no stitch", StandDogEarRunsSwapped, &h,
                            &after);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_CORRUPTED, "refused: a Parcel this Tally lays is not in the Tally before");
    TEST_ASSERT_OK(after, "the empty file empty again, the Tally is written");
    FileRecordRelease(d);
    FileRecordRelease(e);
    d = e = NULL;
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(StandFreshHolds(&m, data), "the data reads its bytes");
out:
    FileRecordRelease(d);
    FileRecordRelease(e);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
#endif
}

static TestResult TestStandDogEarDigestBites(void)
{
#if CONFIG_STRICT
    return test_steps_aside("a STRICT kernel stops at the self-check's DEFECT: a digest changed outside DogEar is "
                            "one it panics on, never one it writes past");
#else
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    StandDogEarHand    h;
    FileSpot           spot = { 0, 0, 0 };
    FileRecord*        d = NULL;
    uint64_t           data = 0;
    error_t            rc = OK, after = OK, refused = OK;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    memset(&h, 0, sizeof(h));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandDogEarTallied(&m, "dogear-digest-bites.bin", &data, &d, NULL),
                "a file of data, a Tally of it, the volume mounted on it");
    TEST_ASSERT(FileRecordFind(d, 0, &spot) && spot.block, "the data's first block");
    h.block = spot.block;
    TEST_ASSERT(TallyDigestFind(spot.block, &h.was, &refused) == TALLY_DIGEST_NAMED,
                "its digest, the Tally's");
    STAND_DOG_EAR_OUTSIDE(TALLY_DIGESTS, ((TallyKey){ spot.block, 0 }),
                          "the key of its DIGESTS element lies outside DogEar");
    rc = StandDogEarLevered("a block's digest no stitch carried", StandDogEarGlossTold, &h, &after);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_CORRUPTED, "refused: the DIGESTS element of the block differs from the Tally "
                   "before's");
    TEST_ASSERT_OK(after, "the Gloss told the digest it had, the Tally is written");
    FileRecordRelease(d);
    d = NULL;
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    tagfs_readahead_invalidate(spot.block);
    TEST_ASSERT(StandFreshHolds(&m, data), "the data reads its bytes, its digests as they were");
out:
    FileRecordRelease(d);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
#endif
}

// ----------------------------------------------------------------------------
// The ribbon a mount replays: every key it changed in DogEar
// ----------------------------------------------------------------------------

static TestResult TestStandDogEarReplayed(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    StandDogEarState   before, after;
    MountBookSaid      said;
    uint64_t           first = 0, data = 0, gone = 0, renamed = 0, born = 0;
    uint32_t           tag = 0;
    bool               kept = false;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandDogEarMake(0, STAND_DOG_EAR_MANY, &first), "files enough for FILES to take two leaves");
    TEST_ASSERT_EQ(tagfs_create_file("dogear-data.bin", NULL, 0, &data), 0, "a file for data is made");
    TEST_ASSERT(StandFreshModelMake(&m, 12u * TAGFS_BLOCK_SIZE) &&
                StandFreshWrite(&m, data, 0, 6u * TAGFS_BLOCK_SIZE, 31) == 6 * TAGFS_BLOCK_SIZE, "and written");
    TEST_ASSERT_OK(TagFSSettle(), "everything settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    /* Every kind of change, a stitch each, into the ribbon past the slot. */
    TEST_ASSERT(StandFreshWrite(&m, data, 2u * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 32) == TAGFS_BLOCK_SIZE &&
                TagFSSettle() == OK, "a block of the data written over, settled");
    TEST_ASSERT(StandFreshWrite(&m, data, 6u * TAGFS_BLOCK_SIZE, 3u * TAGFS_BLOCK_SIZE, 33) == 3 * TAGFS_BLOCK_SIZE &&
                TagFSSettle() == OK, "the data grown, settled");
    renamed = first + STAND_DOG_EAR_MANY / 2u;
    TEST_ASSERT(tagfs_rename_file(renamed, "dogear-renamed.txt", INK_UNWARRANTED) == 0 && TagFSSettle() == OK,
                "a file in the middle renamed, settled");
    TEST_ASSERT(tagfs_tag_intern("dogear:replayed", &tag) == OK && tagfs_add_tag(first, tag, INK_UNWARRANTED) == 0 &&
                TagFSSettle() == OK, "a tag coined and given the first file, settled");
    gone = first + STAND_DOG_EAR_MANY - 1u;
    TEST_ASSERT(tagfs_delete_file(gone, INK_UNWARRANTED) == 0 && TagFSSettle() == OK, "the last file given up, settled");
    TEST_ASSERT(StandStitchSay("dogear: the word", 16, 1, &kept) == OK && kept, "a word said, settled");
    TEST_ASSERT(tagfs_create_file("dogear-born.txt", NULL, 0, &born) == 0 && TagFSSettle() == OK,
                "a file born, settled");
    StandDogEarStateOf(&before);
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn >= 7u, "the ribbon past the Tally replayed, a stitch a change");
    StandDogEarStateOf(&after);
    TEST_ASSERT(StandDogEarSame(&before, &after), "the state the stitches left");
    TEST_ASSERT(StandDogEarHolds(TALLY_FILES, (TallyKey){ data, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ data, 1 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ renamed, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ gone, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ born, 0 }),
                "DogEar holds the key of every file a replayed stitch changed — written over, grown, renamed, "
                "tagged, given up, born");
    TEST_ASSERT(!StandDogEarHolds(TALLY_FILES, (TallyKey){ first + 1u, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ renamed - 1u, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ renamed, 1 }),
                "and not the key of a file no stitch changed, nor a Parcel the rename shared");
    TEST_ASSERT(StandDogEarHolds(TALLY_TAGS, (TallyKey){ tag, 0 }) && StandDogEarHolds(TALLY_USE, (TallyKey){ 0, 0 }),
                "the tag the ribbon coined, and the Use Context it said");
    TEST_ASSERT(DogEarStand(TALLY_DIGESTS, false, NULL, 0) > 0, "and the windows of the ground the data changed");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of the replayed state is written: every element the ribbon changed "
                   "lies in DogEar, and its self-check finds none changed outside it");
    /* Not "DogEar is empty": a Tally's DONE may Glean (binder_glean.c) — the
     * Tally-Quires the tests before left past the pair, emptiest first, put in
     * for the next Tally to lay again. Those are leaves the new Tally names as
     * they lie, in a Quire past the pair it laid in; every key the ribbon
     * changed lies in a leaf it laid anew, which no Glean of it can choose. */
    TEST_ASSERT(!StandDogEarHolds(TALLY_FILES, (TallyKey){ data, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ data, 1 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ renamed, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ gone, 0 }) &&
                !StandDogEarHolds(TALLY_FILES, (TallyKey){ born, 0 }) &&
                !StandDogEarHolds(TALLY_TAGS, (TallyKey){ tag, 0 }) &&
                !StandDogEarHolds(TALLY_USE, (TallyKey){ 0, 0 }) && !DogEarStandLost(false),
                "the Tally whose slice took them let every one of them go");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    StandDogEarStateOf(&after);
    TEST_ASSERT_EQ(said.sewn, 0, "with no stitch to replay past it");
    TEST_ASSERT(StandDogEarSame(&before, &after), "and the state the same");
    TEST_ASSERT(StandFreshHolds(&m, data), "the data reads its bytes");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The stitches of this mount: the tag one coined and the word one said
// ----------------------------------------------------------------------------

static TestResult TestStandDogEarStitched(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandDogEarState   before, after;
    uint64_t           fid  = 0;
    uint32_t           tag  = 0;
    bool               kept = false;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file("dogear-stitched.txt", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "settled, and a Tally of it written");
    TEST_ASSERT(tagfs_tag_intern("dogear:stitched", &tag) == OK && tagfs_add_tag(fid, tag, INK_UNWARRANTED) == 0 &&
                TagFSSettle() == OK, "a tag coined and given the file, settled — the volume not mounted between");
    TEST_ASSERT(StandStitchSay("dogear: stitched", 16, 1, &kept) == OK && kept, "a word said, settled");
    TEST_ASSERT(StandDogEarHolds(TALLY_TAGS, (TallyKey){ tag, 0 }) && StandDogEarHolds(TALLY_USE, (TallyKey){ 0, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ fid, 0 }),
                "DogEar holds what they changed: the tag coined, the Use Context, the file tagged");
    StandDogEarStateOf(&before);
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of these stitches is written: the tag they coined, the word they "
                   "said and the file they tagged lie in DogEar, and its self-check finds nothing changed outside it");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    StandDogEarStateOf(&after);
    TEST_ASSERT(StandDogEarSame(&before, &after), "the state the stitches left");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// Filled and emptied: what the Tally before named and the next does not is in
// DogEar
// ----------------------------------------------------------------------------

static TestResult TestStandDogEarEmptied(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    StandDogEarState   after;
    uint64_t           first = 0, files0 = 0, free0 = 0, free1 = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    files0 = tagfs_get_state()->ledger.total_files;
    free0  = tagfs_get_state()->ledger.free_blocks;
    TEST_ASSERT(StandDogEarMake(0, STAND_DOG_EAR_MANY, &first), "files enough for FILES to take two leaves");
    TEST_ASSERT(StandFreshModelMake(&m, 8u * TAGFS_BLOCK_SIZE), "a model of the data");
    for (uint64_t f = first; f < first + 8u; f++)
        TEST_ASSERT(StandFreshWrite(&m, f, 0, 4u * TAGFS_BLOCK_SIZE + 9u, 40u + f) == 4 * TAGFS_BLOCK_SIZE + 9,
                    "data written into a file of the first leaf");
    TEST_ASSERT_OK(TagFSSettle(), "everything settled");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally of the filled volume is written");
    for (uint64_t f = first; f < first + STAND_DOG_EAR_MANY; f++)
        TEST_ASSERT_EQ(tagfs_delete_file(f, INK_UNWARRANTED), 0, "a file is given up");
    TEST_ASSERT_OK(TagFSSettle(), "every delete settled");
    TEST_ASSERT(StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 1 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ first + STAND_DOG_EAR_MANY / 2u, 0 }) &&
                StandDogEarHolds(TALLY_FILES, (TallyKey){ first + STAND_DOG_EAR_MANY - 1u, 0 }) &&
                DogEarStand(TALLY_DIGESTS, false, NULL, 0) > 0,
                "DogEar holds every file given up, and the windows of the data's ground");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally of the emptied volume is written: every element the Tally "
                   "before named and this one does not lies in DogEar");
    TEST_ASSERT(!StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 0 }), "and it cleared them");
    TEST_ASSERT_OK(StandBinderTallies(1u), "and another, with nothing changed between");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    StandDogEarStateOf(&after);
    free1 = tagfs_get_state()->ledger.free_blocks;
    TestEvidence("free blocks the emptied volume lacks of the start", free0 > free1 ? free0 - free1 : 0);
    TEST_ASSERT_EQ(after.files, files0, "the files the volume began with, and no other");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A copy that would not read: the Tally after it written whole, its self-check
// reading the node through the copy that holds
// ----------------------------------------------------------------------------

static TestResult TestStandDogEarMended(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandDogEarState   before, after;
    MountBookSaid      said;
    TallyPlace         leaf;
    uint64_t           first = 0;
    bool               found = false;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandDogEarMake(0, STAND_DOG_EAR_MANY, &first), "files enough for FILES to take two leaves");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    for (uint32_t i = 1; !found && TallyStandNodeWhere(TALLY_FILES, i, &leaf); i++)
        found = leaf.level == 0 && leaf.has_lo && leaf.has_hi;
    TEST_ASSERT(found && leaf.ptr.copies == 2, "a leaf of FILES between two others, two copies");
    TEST_ASSERT(StandBinderKeep(&b, leaf.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, leaf.ptr.block[0], g_test_output), "and written over with zeros");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.mended >= 1 && said.rotten == 0, "the leaf read through its second copy, its first to mend");
    TEST_ASSERT(StandDogEarHolds(TALLY_FILES, leaf.lo) && !StandDogEarHolds(TALLY_FILES, leaf.hi),
                "its range put in DogEar by the mount that found the copy wanting, and no key past it");
    StandDogEarStateOf(&before);
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is written: its self-check reads the leaf through the copy that holds");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    StandDogEarStateOf(&after);
    TEST_ASSERT_EQ(said.mended, 0, "every copy of every node holds");
    TEST_ASSERT(StandDogEarSame(&before, &after), "the state the same");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A copy of an inner node that would not read: the node laid again, its keys
// not changed
// ----------------------------------------------------------------------------

/* Whether the Foliation holds the inner node of FILES at `at`, marked to be
 * laid again. */
static bool StandDogEarRelaid(const TallyPlace* at)
{
    FoliationNode n;
    for (uint32_t i = 0; FoliationStandNode(TALLY_FILES, i, &n); i++)
        if (n.place.level == at->level && n.place.ptr.copies && n.place.ptr.block[0] == at->ptr.block[0])
            return n.relay;
    return false;
}

static TestResult TestStandDogEarRootMended(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    MountBookSaid      said;
    TallyPlace         root;
    uint64_t           first = 0;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandDogEarMake(0, STAND_DOG_EAR_MANY, &first), "files enough for FILES to take two leaves");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of them written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    TEST_ASSERT(TallyStandNodeWhere(TALLY_FILES, 0, &root) && root.level >= 1 && root.ptr.copies == 2,
                "the FILES root, an inner node of two copies");
    TEST_ASSERT(!StandDogEarRelaid(&root), "the Foliation holds it, not to be laid again");
    TEST_ASSERT(StandBinderKeep(&b, root.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, root.ptr.block[0], g_test_output), "and written over with zeros");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.mended >= 1, "the root read through its second copy");
    TEST_ASSERT(StandDogEarRelaid(&root), "the Foliation marks it to be laid again");
    TEST_ASSERT(!StandDogEarHolds(TALLY_FILES, (TallyKey){ first, 0 }) && DogEarStand(TALLY_FILES, false, NULL, 0) == 0,
                "and DogEar holds no key under it: an inner node's copy changes no element");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.mended, 0, "every copy of every node holds");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A copy of a DIGESTS leaf that would not read, found by a read of a block it
// names: the leaf's range in DogEar
// ----------------------------------------------------------------------------

static TestResult TestStandDogEarDigestMended(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    TallyPlace         leaf;
    FileRecord*        rec   = NULL;
    FileSpot           spot  = { 0, 0, 0 };
    uint64_t           fid   = 0;
    bool               rot   = false, found = false;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT_EQ(tagfs_create_file("dogear-digest.bin", NULL, 0, &fid), 0, "a file for data is made");
    TEST_ASSERT(StandFreshModelMake(&m, 4u * TAGFS_BLOCK_SIZE) &&
                StandFreshWrite(&m, fid, 0, 3u * TAGFS_BLOCK_SIZE, 51) == 3 * TAGFS_BLOCK_SIZE, "and written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally of it is written: its DIGESTS names the data");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(rec && FileRecordFind(rec, 0, &spot) && spot.block, "the data's first block");
    for (uint32_t i = 0; !found && TallyDigestLeafAt(i, &leaf, &rot); i++)
        found = !rot && leaf.level == 0 && (!leaf.has_lo || leaf.lo.k1 <= spot.block) &&
                (!leaf.has_hi || spot.block < leaf.hi.k1);
    TEST_ASSERT(found && leaf.ptr.copies == 2, "the DIGESTS leaf that names it, two copies");
    TEST_ASSERT(StandBinderKeep(&b, leaf.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, leaf.ptr.block[0], g_test_output), "and written over with zeros");
    FileRecordRelease(rec);
    rec = NULL;
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again: the leaves of its DIGESTS kept, none read");
    tagfs_readahead_invalidate(spot.block);
    TEST_ASSERT(StandFreshHolds(&m, fid), "the data reads back: its digest read through the leaf's second copy");
    TEST_ASSERT(StandDogEarHolds(TALLY_DIGESTS, (TallyKey){ spot.block, 0 }),
                "and the leaf's range put in DogEar by that read");
    TEST_ASSERT(TagFSSettle() == OK && BinderRunWhole() == OK, "a stitch of this mount settles, a Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    tagfs_readahead_invalidate(spot.block);
    TEST_ASSERT(StandFreshHolds(&m, fid), "the data reads");
    TEST_ASSERT(!StandDogEarHolds(TALLY_DIGESTS, (TallyKey){ spot.block, 0 }),
                "the leaf written again: no copy of it is wanting");
out:
    FileRecordRelease(rec);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_dog_ear_tests[] = {
    { .name = "stand_dog_ear_strict_bites", .func = TestStandDogEarStrictBites,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_runs_bite", .func = TestStandDogEarRunsBite,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_digest_bites", .func = TestStandDogEarDigestBites,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_replayed", .func = TestStandDogEarReplayed,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_stitched", .func = TestStandDogEarStitched,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_emptied", .func = TestStandDogEarEmptied,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_mended", .func = TestStandDogEarMended,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_root_mended", .func = TestStandDogEarRootMended,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_dog_ear_digest_mended", .func = TestStandDogEarDigestMended,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_dog_ear_suite = {
    g_stand_dog_ear_tests, sizeof(g_stand_dog_ear_tests) / sizeof(g_stand_dog_ear_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
