#include "tests.h"
#include "stand_tests.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../settle/binder.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// A FILES leaf neither copy of which reads: a rotten range (C19) — a case of
// the stand_tally suite (stand_tally_tests.c), and the one of it that writes
// Tallies: a leaf Facsimile cannot read back from an older slot is one every
// slot names as it lies, and on a book whose leaves the tests before laid
// anew — every boot of a stand past its first — none may be. Then one is made
// so, with the kit a test that writes Tallies is run with (stand_binder.h):
// the ring, the ground its slots hold and the ribbon kept as found, and put
// back, the volume mounted on them, once the rot is lifted.
// ============================================================================

/* The first live file whose header (fid, 0) lies in the leaf's [lo, hi): 0
 * when none. Not the lo key's own file: a separator stays where the file it
 * was cut at lay, and on a book whose files were given up since — every boot
 * of a stand past its first — that file may be gone. */
static uint64_t StandTallyLeafFirstFile(const TallyPlace* leaf)
{
    uint64_t from = leaf->has_lo ? leaf->lo.k1 + (leaf->lo.k2 != 0 ? 1u : 0u) : 0, fid = 0;
    if (FileMapRows(tagfs_get_state()->files, from, &fid, 1) != 1)
        return 0;
    if (leaf->has_hi && (fid > leaf->hi.k1 || (fid == leaf->hi.k1 && leaf->hi.k2 == 0)))
        return 0;
    return fid;
}

/* The least Tally a slot that holds may hold — either of an unsure slot's
 * two. A node laid by a Tally no later than it is one every slot names as it
 * lies: no slot holds another copy of its range. */
static uint64_t StandTallyOldestHeld(void)
{
    uint64_t least = UINT64_MAX;
    for (uint32_t i = 0; g_binder.slot && i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        if (!s->holds)
            continue;
        if (s->tally < least)
            least = s->tally;
        if (s->unsure && s->tally2 && s->tally2 < least)
            least = s->tally2;
    }
    return least;
}

/*
 * The last FILES leaf below the root whose range holds a live file's header,
 * and that file, of the leaves every slot names as they lie: a leaf a Tally
 * laid anew since an older slot's has another copy of its range there, and
 * Facsimile reads the range back from that slot (facsimile.c — only a node
 * shared with the Tally stood on rots for both). *below says whether a leaf
 * below the root holds a header memory has a file for: none on a one-leaf tree.
 * The nodes are the walk of the Tally the mount stands on (TallyStandNodeWhere).
 */
static bool StandTallyLeafBelowRoot(TallyPlace* leaf, uint64_t* fid, bool* below)
{
    TallyPlace n;
    uint64_t   oldest = StandTallyOldestHeld();
    bool       found = false;
    *below = false;
    for (uint32_t i = 1; TallyStandNodeWhere(TALLY_FILES, i, &n); i++) {
        uint64_t first = n.level == 0 ? StandTallyLeafFirstFile(&n) : 0;
        *below = *below || first;
        if (first && n.ptr.seq <= oldest) {
            *leaf = n;
            *fid  = first;
            found = true;
        }
    }
    return found;
}

/*
 * ‼ A LEAF NO COPY OF WHICH READS IS A ROTTEN RANGE: MOUNTED, ITS FILES LOCKED,
 * ITS BLOCKS HELD (C19). Where no leaf below the root is one every slot names
 * as it lies, a Glean's Tallies are laid and then one over every slot and one
 * more, nothing changed, so each after the first names every leaf as it lies;
 * the volume mounted again past them, its walk names their nodes. Both copies
 * of the leaf made unreadable, the mount comes up with the range rotten,
 * having asked the medium for each copy once — its run, refused, then each
 * sector alone (book_unit.c BookUnitBySector): the older slots name the leaf
 * where it lies, and Facsimile's look in each takes the answer the walk had
 * (mount/foxing.c), where it read both copies again in every slot of the ring,
 * a drive's recovery of every sector paid three times over. The sectors
 * reading again, it comes up whole — and the ring, its ground and the ribbon
 * go back as the test found them.
 */
TestResult TestStandTallyRottenLeafLocked(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint8_t            seat    = tagfs_get_seat();
    uint64_t           base    = tagfs_get_volume_base();
    const char*        aside   = StandTallyAside();
    TallyPlace         leaf;
    MountBookSaid      said;
    bool               down = false, below = false, found = false, kept = false;
    uint64_t           fid  = 0, sectors = 0;
    uint32_t           asked = 0;
    FileRecord*        rec  = NULL;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(), "the volume mounted afresh: its walk names the newest Tally's nodes");
    found = StandTallyLeafBelowRoot(&leaf, &fid, &below);
    if (!below)
        return test_steps_aside("no leaf below its FILES root holds a live file's header — the tree "
                                "is one leaf, its root: no range lies below a root to rot (a book of "
                                "more files has one — the stand's padded landing)");
    if ((aside = StandBinderAside(&b)) != NULL)
        return test_steps_aside(aside);
    kept = true;
    if (!found) {
        TEST_ASSERT(StandBinderGleaned() && StandBinderTallies(g_binder.ring + 1u) == OK,
                    "a Glean's Tallies laid, then a Tally over each slot of the ring and one more, nothing changed");
        TEST_ASSERT(StandStitchRemount(), "the volume mounted again past them");
        found = StandTallyLeafBelowRoot(&leaf, &fid, &below);
    }
    TEST_ASSERT(found, "a leaf below the root every slot names as it lies");
    rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    TEST_ASSERT(rec != NULL, "a live file whose header the leaf holds");
    FileRecordRelease(rec);

    down = TagFSStandLetGo();
    TEST_ASSERT(down, "the volume is let go of");
    sectors = leaf.ptr.sheets * TAGFS_BLOCK_SECTORS;
    BlockFailArmBadSectors(seat, base + leaf.ptr.block[0] * TAGFS_BLOCK_SECTORS, sectors);
    TEST_ASSERT(BlockFailAddBadSectors(seat, base + leaf.ptr.block[1] * TAGFS_BLOCK_SECTORS, sectors),
                "both copies of the leaf will not read");
    StandTallyArrive();
    TagFSStandHoldAgain();
    down  = false;
    asked = BlockFailSeen();

    TEST_ASSERT(tagfs_get_state()->initialized, "the volume mounts all the same");
    TEST_ASSERT_EQ(asked, 2u * (1u + sectors), "each copy of the leaf asked of the medium once: its run, then "
                                               "each sector alone — no older slot that names the leaf where it lies "
                                               "reads it again");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.rotten, 1, "with one rotten range of FILES");
    TEST_ASSERT(said.locked > 0, "whose files are locked");
    TEST_ASSERT(TagFSFileLocked(fid), "the leaf's first file among them");
    TEST_ASSERT(tagfs_open(fid, TAGFS_HANDLE_READ) == NULL, "and no open of it is handed out");
    TEST_ASSERT(said.held > 0, "and the blocks they named are held");
out:
    BlockFailDisarm();
    if (!down && tagfs_get_state()->initialized)
        TagFSStandLetGo();
    StandTallyArrive();
    MountBookStandSaid(&said);
    if (said.rotten != 0 || TagFSFileLocked(fid))
        verdict = TestFailedAt(__FILE__, __LINE__, "mounted with the sectors reading again, a range "
                               "is still rotten (rotten, 0)", said.rotten, 0);
    StandTallyClose();
    if (kept)
        STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
