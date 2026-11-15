#include "stand_binder.h"
#include "stand_fresh.h"
#include "../ink/fallow.h"
#include "../plot/plot.h"
#include "../settle/binder.h"
#include "../mount/book_mount.h"
#include "../mount/spine_read.h"
#include "../file_map/file_map.h"
#include "../block_map/block_map_derived.h"
#include "../tag_registry/tag_registry.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_binder suite: the kernel writes a whole Tally of a volume and the
// Spine slot that names it
//
// The state after a settled stitch, every tree written whole in two copies,
// the slot of the least seq named over it: mounted again on that slot, the
// volume is the one memory held — files, their ground and bytes, tags, the
// Use Context — with no stitch left to replay. A Tally refused at a node
// leaves the slot it stood on (C7); one cut after its nodes and their flush
// is nobody's, its ground free (C8); a slot torn leaves the one before (C9);
// a flush of the slot refused leaves it whole or not at all (C10); a file of
// many Parcels comes back whole, a cut inside the Tally or not (C23). A copy
// of a node that would not read is written again by the next Tally (C24); a
// node no copy of which reads is named by the next Tally as it lies, its
// range rotten, its files locked and its blocks held as before (§16.4 п.5).
// A Tally is owed once the ribbon past the newest slot holds as many useful
// bytes as a Tally costs (TALLY_SLACK); the Tallies give the ribbon's Quires
// back, so the volume filled and emptied has its ground again, and a tag
// interned at the ribbon's end no longer holds the cycles. Every test puts
// the Spine and the ribbon back as it found them.
// ============================================================================

/* Whether `n` bytes are all zero. */
static bool StandBinderZeros(const uint8_t* p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* What the volume holds, as a mount says it: its live files' print and count,
 * the Use Context's print, the tags of the registry and their bytes. */
typedef struct {
    uint32_t files_print;
    uint64_t files;
    uint32_t use_print;
    uint64_t use_bytes;
    uint64_t tags;
    uint64_t text;
    uint64_t locked;
    uint64_t held;
    uint32_t rotten;
} StandBinderState;

static void StandBinderStateOf(StandBinderState* st)
{
    MountBookSaid said;
    MountBookStandSaid(&said);
    memset(st, 0, sizeof(*st));
    st->files_print = g_book.files_print;
    st->files       = g_book.files;
    st->use_print   = g_book.use_print;
    st->use_bytes   = g_book.use_bytes;
    st->locked      = said.locked;
    st->held        = said.held;
    st->rotten      = said.rotten;
    TagRegistryTextBytes(tagfs_get_state()->registry, &st->tags, &st->text);
}

static bool StandBinderSame(const StandBinderState* a, const StandBinderState* b)
{
    bool same = a->files_print == b->files_print && a->files == b->files && a->use_print == b->use_print &&
                a->use_bytes == b->use_bytes && a->tags == b->tags && a->text == b->text &&
                a->locked == b->locked && a->held == b->held && a->rotten == b->rotten;
    if (!same)
        kprintf("[TESTS] stand_binder: the states differ — files print %08X/%08X, files %llu/%llu, Use %08X/%08X "
                "%llu/%llu bytes, tags %llu/%llu of %llu/%llu bytes, locked %llu/%llu, held %llu/%llu, rotten "
                "%u/%u\n", a->files_print, b->files_print, (unsigned long long)a->files,
                (unsigned long long)b->files, a->use_print, b->use_print, (unsigned long long)a->use_bytes,
                (unsigned long long)b->use_bytes, (unsigned long long)a->tags, (unsigned long long)b->tags,
                (unsigned long long)a->text, (unsigned long long)b->text, (unsigned long long)a->locked,
                (unsigned long long)b->locked, (unsigned long long)a->held, (unsigned long long)b->held,
                a->rotten, b->rotten);
    return same;
}

/* Whether a slot of the ring holds other bytes than the test found it with —
 * the Tally's slot written, whole or in part. */
static bool StandBinderTornWritten(const StandBinder* b)
{
    for (uint32_t i = 0; i < b->slots; i++) {
        if (!StandBinderRead(b, b->spine + i * b->stride, g_test_output))
            return false;
        if (memcmp(g_test_output, b->ring + (uint64_t)i * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE) != 0)
            return true;
    }
    return false;
}

/* The slot of the ring that holds with the greatest seq, read off the medium:
 * its index, its seq and the Tally it names; false when none holds. */
static bool StandBinderNewest(uint32_t* index, uint64_t* seq, uint64_t* tally)
{
    SpineSlot* slots = NULL;
    uint32_t   count = 0;
    char       what[96];
    bool       found = SpineRead(&g_book.frame, &slots, &count, what, sizeof(what)) == OK && count > 0;
    if (found) {
        *index = slots[0].index;
        *seq   = slots[0].seq;
        *tally = slots[0].head.tally_seq;
    }
    SpineLetGo(slots, count);
    return found;
}

/* A few files, their names and tags, data in one of them, and a word: settled,
 * the volume mounted again on it — the state a Tally is then written of. */
static bool StandBinderSow(StandFreshModel* m, uint64_t* data_fid)
{
    static const char* names[] = { "bind-a.txt", "bind-b.txt", "bind-c.bin" };
    uint64_t           fid     = 0;
    bool               kept    = false;
    for (uint32_t i = 0; i < 3u; i++)
        if (tagfs_create_file(names[i], NULL, 0, &fid) != 0)
            return false;
    *data_fid = fid;
    if (!StandFreshModelMake(m, 8u * TAGFS_BLOCK_SIZE) ||
        StandFreshWrite(m, fid, 0, 5u * TAGFS_BLOCK_SIZE + 77u, 21) != 5 * TAGFS_BLOCK_SIZE + 77)
        return false;
    if (StandStitchSay("bind: the word", 14, 1, &kept) != OK || !kept)
        return false;
    /* A Tally of a mount is written only once a stitch of that mount settled
     * (§4.2): one more, carrying nothing, after the mount. */
    return TagFSSettle() == OK && StandStitchRemount() && StandFreshHolds(m, fid) && TagFSSettle() == OK;
}

// ----------------------------------------------------------------------------
// A whole Tally, and the slot that names it
// ----------------------------------------------------------------------------

static TestResult TestStandBinderWhole(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel  m;
    StandBinderState before, after;
    MountBookSaid    said;
    RibbonCursor     c;
    uint64_t         fid = 0, seq = 0, tally = 0;
    uint32_t         index = 0, stood = 0;
    const char*      aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderSow(&m, &fid), "files, data and a word are settled, and the volume mounted again");
    StandBinderStateOf(&before);
    MountBookStandSaid(&said);
    stood = said.slot;
    RibbonCursorStand(&c);
    TEST_ASSERT_OK(BinderRunWhole(), "a whole Tally is written");
    TEST_ASSERT(StandBinderNewest(&index, &seq, &tally), "a slot of the ring holds");
    TEST_ASSERT(index != stood, "the newest is not the one the mount stood on: the Tally's slot");
    TEST_ASSERT(tally > c.last && seq > tally, "it names a Tally past the last stitch, at a seq past the Tally's");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    StandBinderStateOf(&after);
    TEST_ASSERT_EQ(said.slot, index, "on the Tally's slot");
    TEST_ASSERT_EQ(said.sewn, 0, "with no stitch to replay past it");
    TEST_ASSERT_EQ(said.last, c.last, "its chain ending on the stitch the Tally is the state after");
    TEST_ASSERT(StandBinderSame(&before, &after), "and the state it mounts is the one the stitches left");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads its bytes");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles on it");
    TEST_ASSERT(StandStitchRemount(), "and the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.slot == index && said.sewn == 1, "on the same slot, the stitch past the Tally replayed");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// C7: a node refused — the slot stays the one stood on
// ----------------------------------------------------------------------------

static TestResult TestStandBinderNodeRefused(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel  m;
    StandBinderState before, after;
    MountBookSaid    said;
    BinderStandSaid  bs;
    uint64_t         fid = 0, free = 0;
    uint32_t         stood = 0;
    const char*      aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderSow(&m, &fid), "the state is settled and mounted");
    StandBinderStateOf(&before);
    MountBookStandSaid(&said);
    BinderStand(&bs);
    stood = said.slot;
    free  = tagfs_get_state()->free_blocks + bs.pair_left;
    BlockFailArm(b.s.seat, BLOCK_FAIL_WRITE, 0, 0, 1);
    TEST_ASSERT(BinderRunWhole() != OK, "the Tally whose first node the medium refuses is refused");
    BlockFailDisarm();
    BinderStand(&bs);
    TEST_ASSERT_EQ(tagfs_get_state()->free_blocks + bs.pair_left, free,
                   "and the ground its nodes took is given back — the pair of Tally-Quires it opened kept, nodes "
                   "nobody's, for the next Tally to go on in where they began");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    StandBinderStateOf(&after);
    TEST_ASSERT_EQ(said.slot, stood, "on the slot it stood on (C7)");
    TEST_ASSERT(StandBinderSame(&before, &after), "the state the same");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the next Tally is written");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// C8, C9, C10: the slot never written, torn, or its flush refused
// ----------------------------------------------------------------------------

/* What a Tally cut by StandBinderCut left in the session, before the volume
 * was mounted again: the slot it was written over, the free count right after
 * the cut, and the slot the next Tally would be written over. Every free count
 * here holds the open pair of Tally-Quires as free (BinderStandSaid.pair_left):
 * no node lies in it, and a Tally that opened one and was cut keeps it for the
 * next to go on in where its nodes began (binder.c TallyEnded). */
typedef struct {
    uint32_t written;
    uint64_t free;
    uint32_t next;
} StandBinderCutSaid;

/* The Tally begun and ended by `how` (0: stopped before its slot; 1: its slot
 * torn; 2: its slot's flush refused), its answer. */
static error_t StandBinderCutRun(StandBinder* b, uint32_t how)
{
    if (how == 0)
        BinderStandStopAt(TALLY_SLOT);
    else if (how == 1)
        BinderStandTearSlot(4);
    else
        BlockFailArm(b->s.seat, BLOCK_FAIL_FLUSH, 0, 0, 2);
    error_t rc = BinderRunWhole();
    BlockFailDisarm();
    BinderStandStopAt(0);
    BinderStandTearSlot(0);
    TestEvidence("the Tally cut ended with", (uint64_t)rc);
    return rc;
}

/* The state sown and mounted, a Tally begun and ended by `how`, what the
 * session held right after in *cut, and the volume mounted again: the slot it
 * stands on in *slot, the free count it had before the Tally and has now. */
static bool StandBinderCut(StandBinder* b, uint32_t how, StandBinderState* before, StandBinderState* after,
                           uint32_t* stood, uint32_t* slot, uint64_t* free_before, uint64_t* free_after,
                           StandBinderCutSaid* cut)
{
    StandFreshModel m;
    MountBookSaid   said;
    BinderStandSaid bs;
    uint64_t        fid = 0;
    memset(&m, 0, sizeof(m));
    bool ok = StandBinderSow(&m, &fid);
    StandFreshModelLetGo(&m);
    if (!ok)
        return false;
    StandBinderStateOf(before);
    MountBookStandSaid(&said);
    BinderStand(&bs);
    *stood       = said.slot;
    *free_before = tagfs_get_state()->free_blocks + bs.pair_left;
    cut->written = bs.next_slot;
    error_t rc   = StandBinderCutRun(b, how);
    if (how == 0 ? rc != ERR_IO_CANCELLED : rc == OK || rc == ERR_BUSY)
        return false;
    BinderStand(&bs);
    cut->free = tagfs_get_state()->free_blocks + bs.pair_left;
    cut->next = bs.next_slot;
    if (!StandStitchRemount())
        return false;
    MountBookStandSaid(&said);
    BinderStand(&bs);
    *slot       = said.slot;
    *free_after = tagfs_get_state()->free_blocks + bs.pair_left;
    StandBinderStateOf(after);
    return true;
}

static TestResult TestStandBinderUnslotted(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandBinderCutSaid cut;
    StandBinderState before, after;
    uint32_t         stood = 0, slot = 0;
    uint64_t         free_before = 0, free_after = 0;
    const char*      aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderCut(&b, 0, &before, &after, &stood, &slot, &free_before, &free_after, &cut),
                "a Tally cut after its nodes and their flush, the volume mounted again");
    TEST_ASSERT_EQ(cut.free, free_before, "its ground went back as it was cut: no slot can name it");
    TEST_ASSERT_EQ(slot, stood, "the slot stood on is the one before (C8)");
    TEST_ASSERT(StandBinderSame(&before, &after), "the state the same");
    TEST_ASSERT_EQ(free_after, free_before, "and the nodes no slot names are nobody's: the free count as before");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandBinderSlotTorn(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandBinderCutSaid cut;
    StandBinderState before, after;
    uint32_t         stood = 0, slot = 0, index = 0;
    uint64_t         free_before = 0, free_after = 0, seq = 0, tally = 0;
    const char*      aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderCut(&b, 1, &before, &after, &stood, &slot, &free_before, &free_after, &cut),
                "a Tally whose slot is torn, the volume mounted again");
    TEST_ASSERT(cut.free < free_before && cut.next == cut.written,
                "the session held its ground as the torn slot may name it, and would write that slot over first");
    TEST_ASSERT_EQ(slot, stood, "the mount stands on the slot before (C9)");
    TEST_ASSERT(StandBinderNewest(&index, &seq, &tally) && index == stood, "the torn one holds not");
    TEST_ASSERT(StandBinderTornWritten(&b), "and a slot of the ring was written in part: the tear is the Tally's");
    TEST_ASSERT(StandBinderSame(&before, &after), "the state the same");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandBinderSlotFlushRefused(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandBinderCutSaid cut;
    StandBinderState before, after;
    uint32_t         stood = 0, slot = 0;
    uint64_t         free_before = 0, free_after = 0;
    const char*      aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderCut(&b, 2, &before, &after, &stood, &slot, &free_before, &free_after, &cut),
                "a Tally whose slot's flush the medium refuses is refused, the volume mounted again");
    TEST_ASSERT(cut.free < free_before && cut.next == cut.written,
                "the session held its ground as the slot may name it, and would write that slot over first");
    TEST_ASSERT(slot == stood || slot == cut.written, "the mount stands on the slot before, or on the Tally whole");
    TEST_ASSERT(StandBinderSame(&before, &after), "whole or not at all, the state the same (C10)");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and a Tally after it is written");
    TEST_ASSERT(StandStitchRemount(), "and the volume mounts on it");
    StandBinderStateOf(&after);
    TEST_ASSERT(StandBinderSame(&before, &after), "the state the same again");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* A slot torn, and the session goes on without a mount between: the slot
 * that may name either Tally is written over first, and the Tally written
 * over it gives both pairs of nodes back. */
static TestResult TestStandBinderSlotUnsure(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel  m;
    StandBinderState before, after;
    BinderStandSaid  bs;
    MountBookSaid    said;
    uint64_t         fid = 0, free0 = 0, free1 = 0, seq = 0, tally = 0;
    uint32_t         torn = 0, index = 0;
    const char*      aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBinderSow(&m, &fid), "the state is settled and mounted");
    StandBinderStateOf(&before);
    BinderStand(&bs);
    torn  = bs.next_slot;
    free0 = tagfs_get_state()->free_blocks;
    error_t rc = StandBinderCutRun(&b, 1);
    TEST_ASSERT(rc != OK && rc != ERR_BUSY, "a Tally whose slot is torn is refused");
    free1 = tagfs_get_state()->free_blocks;
    TEST_ASSERT(free1 < free0, "and the ground it took stays held: the torn slot may name it");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch settles, no mount between");
    BinderStand(&bs);
    TEST_ASSERT_EQ(bs.next_slot, torn, "the slot that may name either Tally is the one written over next");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally is written, the lag's stitches settled past it");
    TEST_ASSERT(StandBinderNewest(&index, &seq, &tally) && index == torn, "over the torn slot");
    TEST_ASSERT(tagfs_get_state()->free_blocks >= free1,
                "and its DONE gave both pairs back — the torn Tally's and what the slot held before — for the one "
                "it took");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    StandBinderStateOf(&after);
    TEST_ASSERT_EQ(said.slot, torn, "on the Tally written over the torn slot");
    TEST_ASSERT(StandBinderSame(&before, &after) && StandFreshHolds(&m, fid), "the state the same, the file whole");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// C23: a file of many Parcels, whole across a Tally and a cut inside one
// ----------------------------------------------------------------------------

#define STAND_BINDER_STRIDED  300u     /* blocks written one apart: a Strip each, a hole between */

static TestResult TestStandBinderParcels(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel  m;
    StandBinderState before, after;
    uint64_t         fid = 0, strips = 0;
    const char*      aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 2u * STAND_BINDER_STRIDED * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("bind-parcels.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 2u * STAND_BINDER_STRIDED * TAGFS_BLOCK_SIZE), "spanned over a hole");
    for (uint64_t k = 0; k < STAND_BINDER_STRIDED; k++)
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 2u * k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 30u + k),
                       TAGFS_BLOCK_SIZE, "every other block written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted again, a stitch of it settled");
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    strips          = rec ? rec->strip_count : 0;
    FileRecordRelease(rec);
    TestEvidence("Strips of the strided file", strips);
    TEST_ASSERT(strips > 2u * PARCEL_STRIPS_MOST, "the file's Strips fill three Parcels and more");
    StandBinderStateOf(&before);
    BinderStandStopAt(TALLY_FLUSH);
    (void)BinderRunWhole();
    BinderStandStopAt(0);
    TEST_ASSERT(StandStitchRemount(), "a Tally cut inside, before its flush: the volume mounts again");
    StandBinderStateOf(&after);
    TEST_ASSERT(StandBinderSame(&before, &after) && StandFreshHolds(&m, fid), "on the Tally before, the file whole");
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "mounted again, a stitch of it settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a whole Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    StandBinderStateOf(&after);
    rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(rec && rec->strip_count == strips, "the file's Strips as they were");
    FileRecordRelease(rec);
    TEST_ASSERT(StandBinderSame(&before, &after) && StandFreshHolds(&m, fid), "the state the same, the file whole");
out:
    BinderStandStopAt(0);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// C24: a copy that would not read is written again by the next Tally
// ----------------------------------------------------------------------------

static TestResult TestStandBinderHeals(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandBinderState before, after;
    MountBookSaid    said;
    TallyPlace       root;
    const char*      aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(), "the volume mounted afresh");
    TEST_ASSERT(TallyStandNodeWhere(TALLY_FILES, 0, &root) && root.ptr.copies == 2, "the FILES root, two copies");
    TEST_ASSERT(StandBinderKeep(&b, root.ptr.block[0]), "its first copy's first block kept as found");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(&b, root.ptr.block[0], g_test_output), "and written over with zeros");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.mended >= 1, "reading the root's second copy, and saying the first will be mended");
    StandBinderStateOf(&before);
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "a whole Tally is written");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    StandBinderStateOf(&after);
    TEST_ASSERT_EQ(said.mended, 0, "every copy of every node holds: the pair written anew (C24)");
    TEST_ASSERT(StandBinderSame(&before, &after), "the state the same");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// §16.4 п.5: a node no copy of which reads is named as it lies
// ----------------------------------------------------------------------------

static TestResult TestStandBinderRottenNamed(void)
{
    TestResult       verdict = TEST_PASS;
    static StandBinder b;
    StandBinderState before, after;
    MountBookSaid    said;
    TallyPlace       leaf;
    uint64_t         files = 0;
    const char*      aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    verdict = StandBinderRot(&b, &files);
    if (verdict != TEST_PASS)
        goto out;
    StandBinderStateOf(&before);
    TEST_ASSERT(before.rotten == 1 && before.files < files, "its range rotten, the files in it locked");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and a Tally is written over the rotten range");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on it");
    MountBookStandSaid(&said);
    StandBinderStateOf(&after);
    TEST_ASSERT(StandBinderSame(&before, &after), "the same range rotten, the same files locked, the same held");
    TEST_ASSERT(TallyStandNodeWhere(TALLY_FILES, 0, &leaf) && said.sewn == 0, "the new Tally stood on");
    TEST_ASSERT(StandBinderRead(&b, b.kept_block, g_test_output), "the rotten copy read back");
    TEST_ASSERT(StandBinderZeros(g_test_output, TAGFS_BLOCK_SIZE), "and nobody wrote it: the node is named as it lies");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// TALLY_SLACK: a Tally is owed once the ribbon holds what one costs
// ----------------------------------------------------------------------------

static TestResult TestStandBinderSlack(void)
{
    TestResult      verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid said, last;
    bool            kept = false;
    uint32_t        k = 0;
    const char*     aside = StandBinderAside(&b);

    memset(&last, 0, sizeof(last));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(), "the volume mounted afresh");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    BinderStand(&said);
    TEST_ASSERT(!said.owed, "no Tally owed with the ribbon this short");
    for (k = 0; k < 4096u && !said.owed; k++) {
        last = said;
        TEST_ASSERT_OK(StandStitchSayLong(1000u, (char)('a' + k % 26u), 200u + k, &kept), "a word is settled");
        BinderStand(&said);
    }
    TestEvidence("useful bytes past the newest slot's at when a Tally fell owed", said.useful);
    TestEvidence("the bytes the next Tally is reckoned to cost", said.cost);
    TEST_ASSERT(said.owed && said.useful >= TALLY_SLACK * said.cost && last.useful < TALLY_SLACK * last.cost,
                "a Tally falls owed by TALLY_SLACK, and not before");
    TEST_ASSERT_OK(BinderRunWhole(), "it is written");
    BinderStand(&said);
    TEST_ASSERT(said.useful == 0 && (!said.owed || said.demanded),
                "and the ribbon past it owes nothing — a Tally its DONE demanded, Glean's for the Tally-Quires the "
                "tests before left, owed for that alone");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The ribbon's Quires come back with the Tallies
// ----------------------------------------------------------------------------

static TestResult TestStandBinderGroundBack(void)
{
    TestResult      verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid said;
    RibbonCursor    c;
    bool            kept = false;
    uint64_t        free0 = 0, free1 = 0, quires0 = 0;
    const char*     aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(), "the volume mounted afresh");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    free0 = tagfs_get_state()->free_blocks;
    RibbonCursorStand(&c);
    quires0 = c.quires;
    for (uint32_t k = 0; k < 6u; k++)
        TEST_ASSERT_OK(StandStitchSayFilling(c.quire_blocks * TAGFS_BLOCK_SECTORS / 2u, (char)('A' + k), 300u + k,
                                             &kept), "a word half a Quire long is settled");
    RibbonCursorStand(&c);
    TestEvidence("Quires the ribbon ran through", c.quires - quires0);
    TEST_ASSERT(c.quires >= quires0 + 3u, "the ribbon ran through three Quires and more");
    for (uint32_t t = 0; t < 4u; t++) {
        TEST_ASSERT_OK(BinderRunWhole(), "a Tally is written");
        for (uint64_t l = 0; l <= FallowLag(); l++)
            TEST_ASSERT_OK(TagFSSettle(), "and stitches settle past it");
    }
    BinderStand(&said);
    free1 = tagfs_get_state()->free_blocks;
    TestEvidence("Quires of the ribbon the slots hold after four Tallies", said.ribbon_quires);
    TestEvidence("free blocks lost to the book after the Tallies", free0 > free1 ? free0 - free1 : 0);
    TEST_ASSERT(said.ribbon_quires <= 2u, "the slots hold no more than two Quires of the ribbon");
    TEST_ASSERT(free1 + 2u * c.quire_blocks +
                    6u * ((said.tally_blocks + c.quire_blocks - 1u) / c.quire_blocks) * c.quire_blocks >= free0,
                "and the ground the ribbon ran through is back, all but what three Tallies of whole Quires and two "
                "Quires of ribbon hold");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A Tally past two Quires on a fresh ring: the volume filled to its refusal, a
// Tally always possible, and emptied, its ground back (§16.2 п.6)
// ----------------------------------------------------------------------------

/* Files of one name — a tag is never given back (§4.4), a header is: so many
 * headers take FILES past two Quires of a Tally, 128 or so to a leaf. Half of
 * them are made after a Tally, so the Tally of the full volume is twice the
 * one before it: a reserve that kept the last Tally's size alone, its growth
 * since left out, would let the data take the ground it needs (§16.4 п.7). */
#define STAND_BINDER_BIG_FILES  72000u
#define STAND_BINDER_BIG_BATCH  4000u

/* `n` files of the one name made, a batch settled at a time: the first id
 * into *first unless it holds one. */
static TestResult StandBinderMake(uint32_t n, uint64_t* first)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ(tagfs_create_file("bind-big.n", NULL, 0, &fid), 0, "a file is made");
        *first = *first ? *first : fid;
        if (i % STAND_BINDER_BIG_BATCH == STAND_BINDER_BIG_BATCH - 1u)
            TEST_ASSERT_OK(TagFSSettle(), "the files settle a batch at a time");
    }
    TEST_ASSERT_OK(TagFSSettle(), "every file settled");
out:
    return verdict;
}

/* How far the fill has gone is said each time the file passes another
 * STAND_BINDER_FILL_SAY, and — once the volume has less than that free — each
 * time it passes another STAND_BINDER_FILL_SAY_WALL. A volume stretched a
 * gigabyte (logcheck.sh, quill) is filled for minutes on a loaded host, and a
 * log that does not grow for WAIT_QUIET_S is a machine the harness gives up on
 * as hung (stand.sh, wait_for_line): so the fill talks by what it laid, as
 * quilltest_room.c's does, never by the clock — to the wall, where the file
 * grows by what each round of the kernel's wait for room gives back (settle.c,
 * SettleForRoomBook), less than STAND_BINDER_FILL_SAY in all, for minutes. A
 * stretch in which it lays less than STAND_BINDER_FILL_SAY_WALL is the
 * kernel's to say — the Tally its wait writes when it waits on one (binder.h,
 * BinderRoomWait) — and the cycles of a round are said by nothing: one
 * tagfs_write stands in them. */
#define STAND_BINDER_FILL_SAY       (64ull << 20)
#define STAND_BINDER_FILL_SAY_WALL  (1ull << 20)

/* One file written until the volume refuses it: the refusal. */
static error_t StandBinderFill(uint64_t fid)
{
    TagFSFileHandle* h       = tagfs_open(fid, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    uint64_t         written = 0, said = 0;
    PlotSource       src     = { .kernel = g_test_buffer, .bytes = sizeof(g_test_buffer) };
    error_t          rc      = h ? OK : ERR_FILE_NOT_FOUND;
    TestFillAt(g_test_buffer, sizeof(g_test_buffer), 47, 0);
    while (rc == OK) {
        rc = tagfs_write(h, &src, &written);
        uint64_t room = tagfs_get_state()->free_blocks * TAGFS_BLOCK_SIZE;
        uint64_t step = room < STAND_BINDER_FILL_SAY ? STAND_BINDER_FILL_SAY_WALL : STAND_BINDER_FILL_SAY;
        if (h->file_size >= said + step) {
            said = h->file_size - h->file_size % step;
            kprintf("[TESTS] stand_binder_full_back: the file that fills the volume holds %llu MiB\n",
                    (unsigned long long)(h->file_size >> 20));
        }
    }
    if (h)
        tagfs_close(h);
    return rc;
}

static TestResult TestStandBinderFullBack(void)
{
    TestResult      verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid said;
    uint64_t        free0 = 0, first = 0, fill = 0, big = 0, half = 0;
    const char*     aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount() && TagFSSettle() == OK, "the volume mounted afresh, a stitch of it settled");
    uint64_t q = RibbonCursorNow()->quire_blocks;
    free0 = tagfs_get_state()->free_blocks;
    TEST_ASSERT(StandBinderMake(STAND_BINDER_BIG_FILES / 2u, &first) == TEST_PASS, "half the files are made");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally of them is written");
    BinderStand(&said);
    half = said.tally_blocks;
    TEST_ASSERT(StandBinderMake(STAND_BINDER_BIG_FILES / 2u, &first) == TEST_PASS, "and as many again after it");
    TEST_ASSERT_EQ(tagfs_create_file("bind-fill.bin", NULL, 0, &fill), 0, "a file to fill the volume is made");
    TEST_ASSERT_EQ(StandBinderFill(fill), ERR_TAGFS_NO_SPACE, "and written until the volume refuses it");
    TEST_ASSERT_OK(TagFSSettle(), "every file and every byte settled");
    TestEvidence("free blocks the full volume keeps for the book", tagfs_get_state()->free_blocks);
    TEST_ASSERT_OK(StandBinderTallies(3u), "the volume full, a Tally is always possible: three in a row, over the "
                   "two slots that held nothing and the one that held the Tally the volume was made with");
    BinderStand(&said);
    big = said.tally_blocks;
    TestEvidence("blocks a copy of the Tally of half the files took", half);
    TestEvidence("blocks a copy of the Tally of the full volume takes", big);
    TEST_ASSERT(big > 2u * q && big >= half + half / 2u, "a Tally past two Quires, grown by half and more since the "
                "one of half the files");
    TEST_ASSERT_EQ(tagfs_delete_file(fill, INK_UNWARRANTED), 0, "the data is given up");
    for (uint64_t f = first; f < first + STAND_BINDER_BIG_FILES; f++) {
        TEST_ASSERT_EQ(tagfs_delete_file(f, INK_UNWARRANTED), 0, "a file is given up");
        if ((f - first) % STAND_BINDER_BIG_BATCH == STAND_BINDER_BIG_BATCH - 1u)
            TEST_ASSERT_OK(TagFSSettle(), "the deletes settle a batch at a time");
    }
    TEST_ASSERT_OK(TagFSSettle(), "every delete settled");
    TEST_ASSERT_OK(SettleForRoom(), "room is asked for: the ribbon leaves the Quire the deletes lie in");
    TEST_ASSERT_OK(StandBinderTallies(4u), "and Tallies of the emptied volume are written over the whole ring");
    BinderStand(&said);
    uint64_t free1 = tagfs_get_state()->free_blocks;
    uint64_t ring  = 6u * ((said.tally_blocks + q - 1u) / q) * q;
    TestEvidence("free blocks the book keeps past the start, the volume emptied", free0 > free1 ? free0 - free1 : 0);
    TEST_ASSERT(said.tally_blocks < big && said.ribbon_quires <= 2u,
                "the Tallies of the emptied volume are small again, and the slots hold two Quires of ribbon at most");
    TEST_ASSERT(free1 + 2u * q + ring >= free0,
                "and its ground is back — the data's, the big Tallies', the ribbon's — all but what the ring holds "
                "now: three Tallies of whole Quires, both copies, and two Quires of ribbon");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A ribbon that can be given no next_quire: the Tallies it asks for take its
// end in (SETTLE_DEMAND_TALLY), and a tag interned there no longer holds the
// cycles
// ----------------------------------------------------------------------------

#define STAND_BINDER_WORDS  192u    /* a thirty-second of a Quire each: the ribbon runs through six */

/* A word of the test said, a Quire's thirty-second long; refused while the
 * ribbon has no next_quire (§3.6), and said again once the Tallies it asked
 * for, and the lag's stitches past them, gave its Quires back — the first two
 * over slots that held nothing let go of none, and the reserve keeps their
 * ground (BinderRingShort). *refused counts the times it was refused,
 * *tallies the Tallies written. */
static error_t StandBinderWord(uint32_t k, uint64_t q, uint32_t* refused, uint32_t* tallies)
{
    BinderStandSaid said;
    bool            kept = false;
    error_t         rc   = StandStitchSayFilling(q * TAGFS_BLOCK_SECTORS / 32u, (char)('a' + k % 26u), 600u + k, &kept);
    for (uint32_t tries = 0; rc == ERR_TAGFS_NO_SPACE && tries < 8u; tries++) {
        (*refused)++;
        BinderStand(&said);
        if (said.owed && BinderRunWhole() == OK)
            (*tallies)++;
        for (uint64_t l = 0; l <= FallowLag(); l++)
            (void)TagFSSettle();
        rc = StandStitchSayFilling(q * TAGFS_BLOCK_SECTORS / 32u, (char)('a' + k % 26u), 600u + k, &kept);
    }
    return rc;
}

static TestResult TestStandBinderStandingEnd(void)
{
    TestResult      verdict = TEST_PASS;
    static StandBinder b;
    TagSpelling     sp    = { .key = "bind-standing", .key_len = 13, .value = NULL, .value_len = 0 };
    uint32_t        id    = 0, tallies = 0, refused = 0, interned = 0;
    bool            shrunk = false;
    const char*     aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(), "the volume mounted afresh");
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    uint64_t q = RibbonCursorNow()->quire_blocks;
    shrunk = BlockMapDerivedStandShrink(EndpaperKeeps(q) + q / 2u);
    TEST_ASSERT(shrunk, "the volume made small: the book's reserve and half a Quire — room for the Tallies that fill "
                "the ring and one more, and no next_quire past them");
    for (uint32_t k = 0; k < STAND_BINDER_WORDS; k++) {
        uint32_t before = refused;
        TEST_ASSERT_OK(StandBinderWord(k, q, &refused, &tallies), "a word is settled, refused or not first");
        /* A tag interned with no file changed, right where the ribbon ran
         * out: its Coined is laid once the ground came back. */
        if (refused > before && !interned++)
            TEST_ASSERT_OK(TagFSInternSpelling(&sp, &id), "a tag is interned with no file changed");
    }
    TestEvidence("Tallies the ribbon asked for while it ran through six Quires", tallies);
    TEST_ASSERT(refused > 0 && interned, "the ribbon ran out, and a tag was interned there");
    /* As many as it takes the ring to let a Quire go: three where its slots
     * held nothing to let go of, one on a book the stand ran on before. */
    TEST_ASSERT(tallies >= 1u, "the Tallies it asked for gave its Quires back");
    TEST_ASSERT_OK(TagFSSettle(), "and the cycles go on: the ribbon's end does not stand");
out:
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_binder_tests[] = {
    { .name = "stand_binder_whole", .func = TestStandBinderWhole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_node_refused", .func = TestStandBinderNodeRefused,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_unslotted", .func = TestStandBinderUnslotted,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_slot_torn", .func = TestStandBinderSlotTorn,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_slot_flush_refused", .func = TestStandBinderSlotFlushRefused,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_slot_unsure", .func = TestStandBinderSlotUnsure,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_parcels", .func = TestStandBinderParcels,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_heals", .func = TestStandBinderHeals,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_rotten_named", .func = TestStandBinderRottenNamed,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_slack", .func = TestStandBinderSlack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_ground_back", .func = TestStandBinderGroundBack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_full_back", .func = TestStandBinderFullBack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_binder_standing_end", .func = TestStandBinderStandingEnd,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_binder_suite = {
    g_stand_binder_tests, sizeof(g_stand_binder_tests) / sizeof(g_stand_binder_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
