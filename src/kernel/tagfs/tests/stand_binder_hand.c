#include "stand_binder.h"
#include "../ink/fallow.h"
#include "../settle/binder.h"
#include "../mount/book_mount.h"
#include "../mount/away.h"
#include "../mount/tally_read.h"
#include "boardroom.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The hand a test that writes Tallies is run with (stand_binder.h): the
 * Spine's ring and the ground its slots hold kept as found and written back,
 * the ribbon put back, and Tallies laid on the test's word — shared by the
 * suites that write them.
 */

bool StandBinderRead(const StandBinder* b, uint64_t block, uint8_t* out)
{
    return BoardroomRead(b->s.seat, b->s.base + block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, out) == 0;
}

bool StandBinderWrite(const StandBinder* b, uint64_t block, const uint8_t* in)
{
    return BoardroomWrite(b->s.seat, b->s.base + block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, in, NULL) == 0;
}

/* The ring and the ground its slots hold kept as found: NULL, or why not. */
static const char* StandBinderKeepRing(TagFSVolume* vol, StandBinder* b)
{
    b->slots  = vol->book.frame.spine_slots;
    b->spine  = vol->book.frame.spine_block;
    b->stride = vol->book.frame.spine_stride;
    if (b->slots == 0 || b->slots > STAND_BINDER_SLOTS_MOST)
        return "the Spine's ring is not one the stand keeps";
    if (!(b->ring = kmalloc((uint64_t)b->slots * TAGFS_BLOCK_SIZE)))
        return "no memory to keep the ring";
    for (uint32_t i = 0; i < b->slots; i++)
        if (!StandBinderRead(b, b->spine + i * b->stride, b->ring + (uint64_t)i * TAGFS_BLOCK_SIZE))
            return "the ring would not be read";
    BinderStandGivenBegin();
    uint32_t runs = BinderStandGround(vol, NULL, 0);
    b->ground     = kmalloc((uint64_t)(runs ? runs : 1u) * sizeof(Strip));
    b->held       = kmalloc((uint64_t)(runs ? runs : 1u) * sizeof(uint8_t*));
    if (!b->ground || !b->held)
        return "no memory to keep the slots' ground";
    memset(b->held, 0, (uint64_t)(runs ? runs : 1u) * sizeof(uint8_t*));
    /* nground counts what `held` was made for, so it is set only once the
     * count is the one it was made by: the slots' ground grown between the
     * two asks would have the let-go walk past its end. */
    if (BinderStandGround(vol, b->ground, runs) != runs)
        return "the slots' ground changed while it was kept";
    b->nground = runs;
    for (uint32_t i = 0; i < b->nground; i++) {
        uint64_t bytes = (uint64_t)b->ground[i].count * TAGFS_BLOCK_SIZE;
        if (!(b->held[i] = kmalloc(bytes)))
            return "no memory to keep the slots' ground";
        if (BoardroomRead(b->s.seat, b->s.base + b->ground[i].start * TAGFS_BLOCK_SECTORS,
                          b->ground[i].count * TAGFS_BLOCK_SECTORS, b->held[i]) != 0)
            return "the slots' ground would not be read";
    }
    return NULL;
}

/* Why the test stands aside here, or NULL — the ring kept as found. A test
 * that stands aside once the ribbon was kept lays nothing and puts nothing
 * back: what was taken to keep the ring is let go, and the runner gives the
 * machine back its Use Context (StandStitchContextBack). */
const char* StandBinderAside(TagFSVolume* vol, StandBinder* b)
{
    memset(b, 0, sizeof(*b));
    const char* aside = StandStitchAside(vol, &b->s);
    if (aside || !(aside = StandBinderKeepRing(vol, b)))
        return aside;
    for (uint32_t i = 0; b->held && i < b->nground; i++)
        kfree(b->held[i]);
    kfree(b->held);
    kfree(b->ground);
    kfree(b->ring);
    b->held   = NULL;
    b->ground = NULL;
    b->ring   = NULL;
    return aside;
}

/* A block the test changes by hand, kept as found for the put-back. */
bool StandBinderKeep(StandBinder* b, uint64_t block)
{
    if (b->kept || !(b->kept = kmalloc(TAGFS_BLOCK_SIZE)))
        return false;
    b->kept_block = block;
    return StandBinderRead(b, block, b->kept);
}

/* Every Quire the ribbon was given while the test ran written over with zeros
 * — what the ring and its ground as found are written back over after — so no
 * stitch of the test lies past the end of the ribbon the test found: the next
 * mount's direct scan would read it as what settled and is lost (§3.9). */
static bool StandBinderGivenZeroed(const StandBinder* b)
{
    static RibbonQuire given[256];
    uint32_t           n  = BinderStandGiven(given, 256u);
    bool               ok = n <= 256u;
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    for (uint32_t i = 0; ok && i < n; i++)
        for (uint64_t k = 0; ok && k < given[i].blocks; k++)
            ok = StandBinderWrite(b, given[i].block + k, g_test_output);
    return ok;
}

/* The volume let go of, the ring and the kept block written back as found,
 * the ribbon put back (StandStitchPutBack) and the volume mounted on it. */
bool StandBinderPutBack(TagFSVolume* vol, StandBinder* b)
{
    bool ok = true;
    if (vol->state.initialized)
        TagFSStandLetGo(vol);
    ok &= StandBinderGivenZeroed(b);
    for (uint32_t i = 0; b->ring && i < b->slots; i++)
        ok &= StandBinderWrite(b, b->spine + i * b->stride, b->ring + (uint64_t)i * TAGFS_BLOCK_SIZE);
    if (b->kept)
        ok &= StandBinderWrite(b, b->kept_block, b->kept);
    for (uint32_t i = 0; b->held && i < b->nground; i++) {
        if (b->held[i])
            ok &= BoardroomWrite(b->s.seat, b->s.base + b->ground[i].start * TAGFS_BLOCK_SECTORS,
                                 b->ground[i].count * TAGFS_BLOCK_SECTORS, b->held[i], NULL) == 0;
        kfree(b->held[i]);
    }
    kfree(b->held);
    kfree(b->ground);
    b->held    = NULL;
    b->ground  = NULL;
    b->nground = 0;
    kfree(b->ring);
    kfree(b->kept);
    b->ring = b->kept = NULL;
    return StandStitchPutBack(vol, &b->s) && ok;
}

/* `n` Tallies in a row, the lag's stitches settled past each: OK, or the
 * first refusal. A Tally asked for where the last stitch ends on its Quire's
 * last sector is not begun until the next (binder.c, BinderMayNot): a cycle
 * settles one, and it is asked again. */
error_t StandBinderTallies(TagFSVolume* vol, uint32_t n)
{
    error_t rc = OK;
    for (uint32_t t = 0; rc == OK && t < n; t++) {
        rc = BinderRunWhole(vol);
        if (rc == ERR_BUSY && TagFSSettle(vol) == OK)
            rc = BinderRunWhole(vol);
        for (uint64_t l = 0; rc == OK && l <= FallowLag(vol); l++)
            rc = TagFSSettle(vol);
    }
    return rc;
}

/* The Tallies Glean asks for laid (binder_glean.c, BinderGleanAt): a Tally
 * whose DONE chose a Tally-Quire to glean has the next lay that Quire's live
 * nodes again, so a test that holds what a Tally lays anew, or every slot to
 * name a node as it lies, lets them go by first — one a slot of the ring at
 * the most. False when a Tally is refused or one is owed still. */
bool StandBinderGleaned(TagFSVolume* vol)
{
    BinderStandSaid said;
    error_t         rc = OK;
    BinderStand(vol, &said);
    for (uint32_t t = 0; rc == OK && said.demanded && t < vol->book.frame.spine_slots; t++) {
        rc = StandBinderTallies(vol, 1u);
        BinderStand(vol, &said);
    }
    return rc == OK && !said.demanded;
}

#define STAND_BINDER_MANY  140u       /* files enough for FILES to take two leaves and more */

/* A leaf of FILES between two others rotten in both its copies, and the
 * volume mounted again on it writing nothing: *files the files it held
 * before. A range that rots where an older slot still holds it in a node of
 * its own is read back from that slot's Tally and its ribbon (Facsimile,
 * mount/facsimile.c) and rots no more: past the Tallies Glean asks for, which
 * lay the nodes of a Quire it gleans anew, a Tally more for every other slot
 * of the ring, nothing changed between, names the leaves as they lie — every
 * slot then holds the range in the one node. */
TestResult StandBinderRot(TagFSVolume* vol, StandBinder* b, uint64_t* files)
{
    TestResult verdict = TEST_PASS;
    TallyPlace leaf;
    char       name[64];
    uint64_t   fid   = 0;
    bool       found = false;

    for (uint32_t i = 0; i < STAND_BINDER_MANY; i++) {
        ksnprintf(name, sizeof(name), "bind-rot-%04u-of-a-name-long-enough.txt", i);
        TEST_ASSERT_EQ(tagfs_create_file(vol, name, NULL, 0, 0, &fid), 0, "a file is made");
    }
    TEST_ASSERT_OK(TagFSSettle(vol), "the files settled");
    TEST_ASSERT_OK(BinderRunWhole(vol), "a Tally of them written");
    TEST_ASSERT(StandBinderGleaned(vol), "and the Tallies Glean asks for after it");
    TEST_ASSERT_OK(StandBinderTallies(vol, vol->book.frame.spine_slots - 1u),
                   "a Tally more for every other slot, each naming the leaves as they lie");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounts on it");
    *files = vol->book.files;
    for (uint32_t i = 1; !found && TallyStandNodeWhere(TALLY_FILES, i, &leaf); i++)
        found = leaf.level == 0 && leaf.has_lo && leaf.has_hi;
    TEST_ASSERT(found && leaf.ptr.copies == 2, "a leaf of FILES between two others, two copies");
    TEST_ASSERT(StandBinderKeep(b, leaf.ptr.block[0]), "kept");
    memset(g_test_output, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandBinderWrite(b, leaf.ptr.block[0], g_test_output) &&
                StandBinderWrite(b, leaf.ptr.block[1], g_test_output), "both its copies written over with zeros");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounts again");
out:
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
