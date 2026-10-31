#include "stand_binder.h"
#include "../ink/fallow.h"
#include "../settle/binder.h"
#include "../mount/book_mount.h"
#include "../mount/away.h"
#include "boardroom.h"

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

/* Why the test stands aside here, or NULL — the ring kept as found. */
const char* StandBinderAside(StandBinder* b)
{
    memset(b, 0, sizeof(*b));
    const char* aside = StandStitchAside(&b->s);
    if (aside)
        return aside;
    b->slots  = g_book.frame.spine_slots;
    b->spine  = g_book.frame.spine_block;
    b->stride = g_book.frame.spine_stride;
    if (b->slots == 0 || b->slots > STAND_BINDER_SLOTS_MOST)
        return "the Spine's ring is not one the stand keeps";
    if (!(b->ring = kmalloc((uint64_t)b->slots * TAGFS_BLOCK_SIZE)))
        return "no memory to keep the ring";
    for (uint32_t i = 0; i < b->slots; i++)
        if (!StandBinderRead(b, b->spine + i * b->stride, b->ring + (uint64_t)i * TAGFS_BLOCK_SIZE))
            return "the ring would not be read";
    BinderStandGivenBegin();
    uint32_t runs = BinderStandGround(NULL, 0);
    b->ground     = kmalloc((uint64_t)(runs ? runs : 1u) * sizeof(Strip));
    b->held       = kmalloc((uint64_t)(runs ? runs : 1u) * sizeof(uint8_t*));
    if (!b->ground || !b->held)
        return "no memory to keep the slots' ground";
    memset(b->held, 0, (uint64_t)(runs ? runs : 1u) * sizeof(uint8_t*));
    b->nground = BinderStandGround(b->ground, runs);
    if (b->nground != runs)
        return "the slots' ground changed while it was kept";
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
bool StandBinderPutBack(StandBinder* b)
{
    bool ok = true;
    if (tagfs_get_state()->initialized)
        TagFSStandLetGo();
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
    return StandStitchPutBack(&b->s) && ok;
}

/* `n` Tallies in a row, the lag's stitches settled past each: OK, or the
 * first refusal. A Tally asked for where the last stitch ends on its Quire's
 * last sector is not begun until the next (binder.c, BinderMayNot): a cycle
 * settles one, and it is asked again. */
error_t StandBinderTallies(uint32_t n)
{
    error_t rc = OK;
    for (uint32_t t = 0; rc == OK && t < n; t++) {
        rc = BinderRunWhole();
        if (rc == ERR_BUSY && TagFSSettle() == OK)
            rc = BinderRunWhole();
        for (uint64_t l = 0; rc == OK && l <= FallowLag(); l++)
            rc = TagFSSettle();
    }
    return rc;
}

/* The Tallies Glean asks for laid (binder_glean.c, BinderGleanAt): a Tally
 * whose DONE chose a Tally-Quire to glean has the next lay that Quire's live
 * nodes again, so a test that holds what a Tally lays anew, or every slot to
 * name a node as it lies, lets them go by first — one a slot of the ring at
 * the most. False when a Tally is refused or one is owed still. */
bool StandBinderGleaned(void)
{
    BinderStandSaid said;
    error_t         rc = OK;
    BinderStand(&said);
    for (uint32_t t = 0; rc == OK && said.demanded && t < g_book.frame.spine_slots; t++) {
        rc = StandBinderTallies(1u);
        BinderStand(&said);
    }
    return rc == OK && !said.demanded;
}

#endif /* CONFIG_VOLUME_TESTS */
