#include "book_mount.h"
#include "ground_grow.h"
#include "../block_map/block_map_derived.h"
#include "boardroom.h"

/*
 * The volume taking the ground behind it (book_mount.h, MountBookGrow).
 *
 * ‼ ONLY THE PROLOGUE MOVES. A book counts its blocks from the volume's block
 * 0 and keeps no map of them on the medium: its FRAME names where the Spine
 * and the data run begin and nothing about where the volume ends — that is
 * the far Deed's place, `tail_sector` (volume_deed.h, "WHY THERE IS NO LENGTH
 * HERE"). So a growth writes `sectors` and `tail_sector` and not a byte of the
 * book, and the data run ends at the new far Deed: every block number the
 * book names means what it meant.
 *
 * ‼ AFTER THE BOOK, AND ONLY BY A MOUNT THAT WRITES IT. The mount reads the
 * whole book first — its slots, its Tally, its ribbon, the census — and writes
 * nothing until every verdict is in (C12): a volume under glass, for whatever
 * reason the ground or the book gave, takes no ground, and neither does one
 * whose medium refuses writes as protected.
 *
 * ‼ THE NEW GROUND IS PROVEN NO ONE'S BEFORE IT IS TAKEN. A book has no
 * bitmap, and the census is its account of every owner: no owner but the far
 * Deed may name a block at or past the old far Deed's (GroundCensusReach) —
 * as the owner names it, before a run is cut to the data run: a file locked
 * for a run past the far Deed holds only what lies inside, and the rest is
 * still its (tally_leaves.c BookFileRun, GroundCensusNamed). A rotten FILES
 * range, whose files' runs no name that reads gives, leaves that unproven —
 * so neither grows, and the reason is said.
 *
 * ‼ NOTHING CHANGES UNLESS ALL OF IT CAN. The wider block map is made before a
 * byte is written (BlockMapDerivedWidenMake); the two copies of the Deed are
 * written far end then head (ground_grow.h); only once the
 * head is written is the map widened and the data run ended at the new far
 * Deed. A growth cut between its two writes leaves the volume its old size —
 * the head is what the next mount stands on — and that mount grows it again
 * from the head's own numbers.
 *
 * ‼ ONE CORE. It runs inside the mount with the door shut: the boot mount is
 * the BSP's alone, a later one is under attending_take().
 */

/* Why this volume takes none of the ground behind it, or NULL when it may. */
static const char* BookGrowRefused(uint8_t seat, char* words, size_t room)
{
    if (TagFSIsUnderGlass())
        return "it may only be read (under glass), and a volume that may only be read writes no Deed";
    if (BoardroomSeatRefusesWrites(seat))
        return "its medium refuses writes as protected";
    if (g_book.files_rot)
        return "a rotten range of its FILES may name ground past its end, and no name that reads says it "
               "does not";
    if (g_book.reach > g_book.data_end) {
        ksnprintf(words, room, "its book names ground up to block %llu, past the far Deed's block %llu — that "
                  "ground is not blank", (unsigned long long)g_book.reach - 1u, (unsigned long long)g_book.data_end);
        return words;
    }
    return NULL;
}

/* The Deed at the head read again — it is what the growth amends, and the
 * mount let go of it once it stood — and held to the one it stood on. True,
 * *head the caller's to release. */
static bool BookGrowHeadRead(uint8_t seat, DeedCopy* head)
{
    TagFSState*  fs     = tagfs_get_state();
    MediumGround ground = { .start_sector = tagfs_get_volume_base(),
                            .sectors      = g_book.ground,
                            .origin       = GROUND_FROM_MBR,
                            .entry        = 0 };
    if (DeedReadHead(seat, &ground, head) != OK) {
        kprintf("[TagFS] seat %u: the deed at the head of this volume would not read again — nothing is "
                "taken\n", seat);
        return false;
    }
    if (memcmp(head->head.uuid, fs->uuid, 16) != 0 ||
        head->head.sectors != fs->volume_bytes / VOLUME_DEED_SECTOR_BYTES ||
        head->head.tail_sector != g_book.data_end * VOLUME_BLOCK_SECTORS) {
        kprintf("[TagFS] seat %u: the deed at the head of this volume is not the one this mount stood on — "
                "nothing is taken\n", seat);
        DeedRelease(head);
        return false;
    }
    return true;
}

/* The far Deed and then the head written for a volume of `sectors` whose far
 * copy sits at `tail` — the prologue the only change. True once the head is. */
static bool BookGrowDeedsWritten(uint8_t seat, uint64_t sectors, uint64_t tail)
{
    DeedCopy head;
    DeedPen  pen;
    bool     written = false;
    if (!BookGrowHeadRead(seat, &head))
        return false;
    uint8_t* keep = kmalloc(head.raw_bytes);
    if (!keep) {
        kprintf("[TagFS] seat %u: there is no memory to amend the deed of this volume — nothing is taken\n",
                seat);
        DeedRelease(&head);
        return false;
    }
    memcpy(keep, head.raw, head.raw_bytes);
    if (DeedPenTake(&pen, head.raw, head.raw_bytes) != 0)
        kprintf("[TagFS] seat %u: the deed at the head of this volume read and its pen would not take it — "
                "nothing is taken\n", seat);
    else
        written = GroundGrowDeedsWritten(seat, &head, &pen, keep, sectors, tail);
    kfree(keep);
    DeedRelease(&head);
    return written;
}

void MountBookGrow(uint8_t seat)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    have = fs->volume_bytes / VOLUME_DEED_SECTOR_BYTES;
    char        words[160];
    const char* why;

    /* A volume standing on its far copy alone is not grown: its head is gone,
     * and the head is the commit point. */
    if (!g_book.on_head || !GroundGrowRoom(seat, g_book.ground, have))
        return;
    if ((why = BookGrowRefused(seat, words, sizeof(words))) != NULL) {
        kprintf("[TagFS] seat %u: this volume runs %llu sectors and the ground under it runs %llu — it takes "
                "none of the rest: %s\n", seat, (unsigned long long)have, (unsigned long long)g_book.ground, why);
        return;
    }

    /* new_end > old_end: the ground runs more than a block past `have`
     * (GroundGrowRoom), and the old far Deed lies inside `have` (deed.c). */
    uint64_t blocks   = g_book.ground / VOLUME_BLOCK_SECTORS;
    uint64_t new_end  = blocks - 1u;                 /* the far Deed's block, as it is the last */
    uint64_t sectors  = blocks * VOLUME_BLOCK_SECTORS;
    uint64_t old_end  = g_book.data_end;
    uint64_t old_tail = old_end * VOLUME_BLOCK_SECTORS;
    kprintf("[TagFS] seat %u: this volume runs %llu sectors and the ground under it runs %llu — it takes the "
            "rest\n", seat, (unsigned long long)have, (unsigned long long)g_book.ground);

    /* The old far Deed is erased once the head commits — by a buffer taken
     * now, so a growth never commits to leave a Deed of the old length inside
     * its data run for want of memory (ground_grow.h). */
    BlockMapWidening wide;
    const char*      what  = NULL;
    uint8_t*         blank = kmalloc(DEED_PEN_BYTES);
    if (!blank) {
        kprintf("[TagFS] seat %u: the ground is not taken — there is no memory to erase the old far Deed with\n",
                seat);
        return;
    }
    if (BlockMapDerivedWidenMake(new_end, &wide, &what) != OK) {
        kprintf("[TagFS] seat %u: the ground is not taken — %s\n", seat, what);
        kfree(blank);
        return;
    }
    if (!BookGrowDeedsWritten(seat, sectors, new_end * VOLUME_BLOCK_SECTORS)) {
        BlockMapDerivedWidenDrop(&wide);
        kfree(blank);
        return;
    }

    /* Committed. What follows is memory, and the old far copy erased. */
    BlockMapDerivedWidenPut(&wide);
    g_book.data_end  = new_end;
    fs->block_end    = new_end;
    fs->volume_bytes = sectors * VOLUME_DEED_SECTOR_BYTES;
    kprintf("[TagFS] seat %u: the volume now runs %llu sectors; its data run is %llu blocks, %llu more than it "
            "had, and its far copy has moved to sector %llu\n", seat, (unsigned long long)sectors,
            (unsigned long long)(new_end - g_book.frame.data_block), (unsigned long long)(new_end - old_end),
            (unsigned long long)(new_end * VOLUME_BLOCK_SECTORS));
    GroundGrowOldTailErased(seat, old_tail, blank);
    kfree(blank);
}
