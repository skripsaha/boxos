#include "ground_grow.h"
#include "boardroom.h"

/*
 * What a volume does to take the ground behind it (ground_grow.h): the
 * questions asked of the medium before anything is measured, the Deed written
 * at the new far end and then at the head, and the old far copy erased. The
 * census's proof and the block map are book_grow.c's.
 */

bool GroundGrowRoom(uint8_t seat, uint64_t ground_sectors, uint64_t have)
{
    /* Not a byte of room, or not a whole block of it: nothing to take, and it
     * is said, because "checked and there is nothing" and "never checked" have
     * to be tellable apart from a screen. */
    if (ground_sectors <= have + TAGFS_BLOCK_SECTORS) {
        kprintf("[TagFS] seat %u: the volume fills the ground it was given — "
                "%llu sectors, all of it\n", seat, (unsigned long long)have);
        return false;
    }

    /*
     * ‼ THE MEDIUM HAS TO SAY HOW FAR IT RUNS, OR NOTHING IS TAKEN.
     *
     * A partition entry is bounded against the medium's own length only when
     * the medium answered (ground.c). A medium that will not say leaves the
     * entry unchecked — and reading past the end of a device is recoverable,
     * while writing a Deed that permanently claims ground which does not exist
     * is not: the volume then fails its own extent check on every boot after,
     * bricked by its own commit.
     */
    uint64_t medium = BoardroomSeatSectors(seat);
    if (medium == 0) {
        kprintf("[TagFS] seat %u: there is ground behind this volume and the "
                "medium will not say how far it runs — nothing is taken, "
                "because a partition table is the only thing claiming that "
                "ground is there\n", seat);
        return false;
    }
    if (tagfs_get_volume_base() + ground_sectors > medium) {
        kprintf("[TagFS] seat %u: the ground behind this volume ends at sector "
                "%llu and the medium ends at %llu — nothing is taken\n", seat,
                (unsigned long long)(tagfs_get_volume_base() + ground_sectors),
                (unsigned long long)medium);
        return false;
    }
    return true;
}

bool GroundGrowDeedsWritten(uint8_t seat, DeedCopy *head, DeedPen *pen, const uint8_t *keep,
                            uint64_t new_sectors, uint64_t new_tail)
{
    DeedPenSetGround(pen, new_sectors, new_tail);

    /* --- The far copy first, and it must read back ---------------------- */
    DeedPenSetRole(pen, VOLUME_DEED_ROLE_TAIL);
    DeedPenSeal(pen);

    bool far_end_took_it =
        tagfs_volume_write(new_tail, DEED_PEN_SECTORS, head->raw) == 0 &&
        BoardroomFlush(seat) == 0;

    if (far_end_took_it) {
        /* Read it back off the medium. This is the one live proof that the
         * ground the partition table claims really is there and answers — a
         * table can lie, a device that returns the bytes cannot. */
        MediumGround grown_ground = { .start_sector = tagfs_get_volume_base(),
                                      .sectors      = new_sectors,
                                      .origin       = GROUND_FROM_MBR,
                                      .entry        = 0 };
        DeedCopy proof;
        DeedCopy asif = *head;
        asif.head.sectors     = new_sectors;
        asif.head.tail_sector = new_tail;
        if (DeedReadTail(seat, &grown_ground, &asif, &proof) == OK) {
            DeedRelease(&proof);
        } else {
            far_end_took_it = false;
        }
    }

    if (!far_end_took_it) {
        memcpy(head->raw, keep, head->raw_bytes);
        kprintf("[TagFS] seat %u: the new far end at sector %llu would not take "
                "its deed and give it back — the volume is left exactly as it "
                "was\n", seat, (unsigned long long)new_tail);
        return false;
    }

    /* --- The head. Before this line the volume is old; after it, new. -- */
    DeedPenSetRole(pen, VOLUME_DEED_ROLE_HEAD);
    DeedPenSeal(pen);

    if (tagfs_volume_write(0, DEED_PEN_SECTORS, head->raw) != 0 ||
        BoardroomFlush(seat) != 0) {
        memcpy(head->raw, keep, head->raw_bytes);
        kprintf("[TagFS] seat %u: the deed at the head would not be written — "
                "the volume stands at its old size and will try again on the "
                "next mount\n", seat);
        return false;
    }
    return true;
}

void GroundGrowOldTailErased(uint8_t seat, uint64_t old_tail, uint8_t *blank)
{
    /*
     * ‼ THE OLD FAR COPY IS ERASED, AND THAT IS SAFETY AND NOT TIDINESS.
     *
     * The block the old tail sat in is inside the data run now and free, so
     * it will be written over by the first file that lands on it. Until then
     * it is a checksum-valid Deed that states the OLD length — and
     * DeedReadTailAlone finds a tail by reading the last block of the ground
     * it is given. Image this volume onto a partition that happens to end
     * there, lose the head, and the machine mounts on a stale deed believing
     * an old data run: block numbers handed out over files.
     */
    memset(blank, 0, DEED_PEN_BYTES);
    if (tagfs_volume_write(old_tail, DEED_PEN_SECTORS, blank) == 0) {
        kprintf("[TagFS] seat %u: the deed that used to sit at sector %llu "
                "is inside the data run now and has been erased\n",
                seat, (unsigned long long)old_tail);
    } else {
        kprintf("[TagFS] seat %u: the deed that used to sit at sector %llu "
                "is inside the data run now and would not erase — it will "
                "be written over by the first file that lands there\n",
                seat, (unsigned long long)old_tail);
    }
    /* The one flush on this path whose answer is deliberately dropped, and it
     * stays dropped. The volume was committed at the head-deed write; what is
     * done since is bookkeeping whose own failure has already been said out
     * loud two lines up, and which the next file to land on that block undoes
     * anyway. There is no claim here for a refused flush to falsify. */
    (void)BoardroomFlush(seat);
}
