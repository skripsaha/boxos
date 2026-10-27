#include "ground_grow.h"

/*
 * A format-v1 volume taking the ground behind it (volume_take_more_ground):
 * its Deed grown at the far end and then at the head, the new ground first
 * proven blank by its bitmap. Moved here out of tagfs.c whole — format v1's
 * growth stands on its LAYOUT and its bitmap, which a format-v2 volume has
 * neither of; what the two formats share, the medium's questions and the
 * order of the Deed's two copies, is ground_grow.c's, and a book grows by its
 * prologue alone, after it is read (book_grow.c, spec_v2 §16.1 п.4).
 */

/* A Deed occupies one block wherever it sits — the pen says how many sectors
 * that is, so the tool and the kernel cannot come to disagree about it. */
#define VOLUME_DEED_BLOCKS  1u

/* How many data blocks a bitmap of this many blocks can carry a bit for. */
static uint64_t bitmap_covers_blocks(uint32_t bitmap_blocks)
{
    return (uint64_t)bitmap_blocks * (uint64_t)TAGFS_BLOCK_SIZE * 8u;
}

/*
 * Every bit for the blocks in [from, to) must read zero.
 *
 * This is a PROOF, not a write. The bits past the data run were zeroed when
 * the volume was made and nothing has been allowed to touch them since — every
 * writer of a bit is bounded by data_blocks — so a bit that is set there is
 * the medium telling us this ground is not what the partition table says it
 * is. Reading is enough to find that out, and reading cannot destroy anything,
 * which is why growth never writes a bitmap.
 *
 * Read a block at a time: the range can be two megabytes on a volume growing
 * into a 64 GB stick, and a mount is not the place to ask a kernel heap whose
 * floor is two megabytes for all of it at once.
 */
static bool ground_ahead_is_blank(uint32_t from, uint32_t to, uint32_t *out_set)
{
    *out_set = 0;
    if (to <= from) return true;

    uint8_t *buf = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    if (!buf) return false;

    uint32_t first_byte = from / 8u;
    uint32_t last_byte  = (to + 7u) / 8u;         /* exclusive */
    uint64_t base       = (uint64_t)TagFSLayout()->block_bitmap_block *
                          TAGFS_BLOCK_SECTORS;

    for (uint32_t off = first_byte / TAGFS_BLOCK_SIZE * TAGFS_BLOCK_SIZE;
         off < last_byte; off += TAGFS_BLOCK_SIZE) {
        if (tagfs_volume_read(base + off / TAGFS_SECTOR_SIZE,
                              TAGFS_BLOCK_SECTORS, buf) != 0) {
            /* Nothing was learned, so nothing is reported: a partial tally
             * from the blocks read before the failure would have the caller
             * saying the bitmap has blocks marked used when what happened is
             * that the medium would not answer. */
            *out_set = 0;
            kfree(buf);
            return false;
        }
        for (uint32_t b = 0; b < TAGFS_BLOCK_SIZE; b++) {
            uint32_t byte_index = off + b;
            if (byte_index < first_byte || byte_index >= last_byte) continue;
            if (buf[b] == 0) continue;
            for (int bit = 0; bit < 8; bit++) {
                uint32_t block = byte_index * 8u + (uint32_t)bit;
                if (block < from || block >= to) continue;
                if (buf[b] & (1u << bit)) (*out_set)++;
            }
        }
    }

    kfree(buf);
    return *out_set == 0;
}

/* What a growth takes, measured before a byte is written: the volume's
 * blocks and its data run's once grown, its data run's now, and where the
 * grown volume ends and its far copy sits. */
typedef struct {
    uint64_t vol_want;
    uint64_t data_want;
    uint32_t data_now;
    uint64_t new_sectors;
    uint64_t new_tail;
} GroundV1Take;

/* Whether there is ground behind the volume that it may take, and how much
 * (into *t): each reason it takes none said. */
static bool GroundV1Measured(uint8_t seat, uint64_t ground_sectors, const DeedCopy *head,
                             GroundV1Take *t)
{
    uint64_t have = head->head.sectors;

    if (!GroundGrowRoom(seat, ground_sectors, have))
        return false;

    uint32_t data_block  = TagFSLayout()->data_block;
    uint32_t data_now    = TagFSLayout()->data_blocks;
    uint64_t vol_want    = ground_sectors / TAGFS_BLOCK_SECTORS;
    if (vol_want <= (uint64_t)data_block + VOLUME_DEED_BLOCKS) return false;

    uint64_t data_want = vol_want - data_block - VOLUME_DEED_BLOCKS;

    /*
     * ‼ CLAMPED BY THE BITMAP, AND THE VOLUME SAYS SO.
     *
     * The bitmap lies in front of the data run, so it cannot be made bigger
     * without moving data_block — see above. mkfs lays it down for a growth
     * ceiling, and this is the volume meeting that ceiling: it takes what its
     * bitmap can account for and leaves the rest, rather than handing out
     * blocks it has nowhere to record.
     */
    uint64_t cover = bitmap_covers_blocks(TagFSLayout()->block_bitmap_blocks);
    bool clamped = false;
    if (data_want > cover) {
        data_want = cover;
        vol_want  = (uint64_t)data_block + data_want + VOLUME_DEED_BLOCKS;
        clamped   = true;
    }

    if (data_want <= (uint64_t)data_now) {
        kprintf("[TagFS] seat %u: the volume fills what its bitmap can account "
                "for — %u data blocks\n", seat, data_now);
        return false;
    }
    /* The layout counts blocks in 32 bits; a volume that outgrew that is a
     * volume this format cannot describe, and it stops here rather than
     * wrapping. */
    if (vol_want > 0xFFFFFFFFull || data_want > 0xFFFFFFFFull) return false;

    uint64_t new_sectors = vol_want * TAGFS_BLOCK_SECTORS;
    uint64_t new_tail    = (vol_want - VOLUME_DEED_BLOCKS) * TAGFS_BLOCK_SECTORS;

    kprintf("[TagFS] seat %u: this volume runs %llu sectors and the ground "
            "under it runs %llu — it takes the rest\n", seat,
            (unsigned long long)have, (unsigned long long)ground_sectors);
    if (clamped) {
        kprintf("[TagFS] seat %u: its block bitmap accounts for %llu blocks, so "
                "it takes %llu of them and leaves %llu sectors unclaimed — "
                "moving the bitmap would move every block number on it\n",
                seat, (unsigned long long)cover, (unsigned long long)data_want,
                (unsigned long long)(ground_sectors - new_sectors));
    }

    t->vol_want    = vol_want;
    t->data_want   = data_want;
    t->data_now    = data_now;
    t->new_sectors = new_sectors;
    t->new_tail    = new_tail;
    return true;
}

/* Steps 2 and 3 below: the Deed amended into *grown — its LAYOUT here, its
 * prologue and its two copies by ground_grow.c — the far copy written at the
 * new end and read back, then its head, the commit. False, the Deed in `head`
 * put back as it was and why said, when either did not take. */
static bool GroundV1DeedsWritten(uint8_t seat, DeedCopy *head, const GroundV1Take *t,
                                 VolumeLayout *grown)
{
    /* --- 2. The Deed, amended ------------------------------------------ */
    uint8_t *keep = (uint8_t *)kmalloc(head->raw_bytes);
    if (!keep) return false;
    memcpy(keep, head->raw, head->raw_bytes);

    DeedPen pen;
    if (DeedPenTake(&pen, head->raw, head->raw_bytes) != 0) { kfree(keep); return false; }

    uint16_t lay_bytes = 0;
    void *live = DeedPenFind(&pen, VOLUME_STAMP_LAYOUT, &lay_bytes);
    if (!live || lay_bytes < sizeof(VolumeLayout)) { kfree(keep); return false; }

    /*
     * ‼ TWO FIELDS IN HERE ARE COPIED FORWARD AND MEAN NOTHING.
     *
     * `file_table_blocks` and `metadata_pool_blocks` are carried out of the
     * Deed at mount and put back unchanged here, and both chains have been
     * lengthening underneath them since the volume was made. Nothing reads
     * either one any more — the mount walks both chains to their end — so what
     * is re-committed is a stale number, not a wrong decision.
     *
     * They are written rather than zeroed because the stamp is shared: an
     * older kernel reading this Deed finds the fields where it expects them,
     * and a 1 it once believed is no worse than the 0 it would find instead.
     * Stating the truth would mean asking the two subsystems how long their
     * chains are at this instant and committing that into a record whose whole
     * point is that it does not change — which is the fault this repair
     * removed, put back in a different field.
     */
    memcpy(grown, TagFSLayout(), sizeof(*grown));
    grown->total_blocks = t->vol_want;
    grown->data_blocks  = (uint32_t)t->data_want;
    /* Only the fields this build knows: a stamp longer than the struct keeps
     * whatever a later mkfs put after them. */
    memcpy(live, grown, sizeof(*grown));

    bool written = GroundGrowDeedsWritten(seat, head, &pen, keep, t->new_sectors, t->new_tail);
    kfree(keep);
    return written;
}

/*
 * ‼ THE VOLUME TAKES THE GROUND IT WAS GIVEN.
 *
 * An image is written to a medium with dd, and the medium is nearly always
 * bigger than the image. Until now the volume simply did not use the rest: a
 * 90 MiB image on a 64 GB stick left 63 GB of it dark, forever, with nothing
 * saying so. A volume that cannot take the ground under it is a volume for
 * image files, not for a machine somebody boots.
 *
 * So on every mount the two numbers are compared — how far the partition runs
 * and how far the Deed says the volume runs — and when there is ground behind
 * it, the volume takes it. That is three numbers in the Deed and nothing else:
 * `sectors`, `tail_sector`, and the LAYOUT stamp's `total_blocks`/`data_blocks`.
 *
 * ‼ data_block DOES NOT MOVE, and that is the whole reason this is safe. A
 * block number in a file's metadata is an index into the data run; move where
 * the run starts and every number on the volume means something else. The run
 * only ever gets LONGER, at the far end, where nothing was.
 *
 * ‼ THE ORDER IS THE CRASH SAFETY, and there is nothing else protecting this.
 *
 *   1. prove the new range is blank (a read — nothing is changed)
 *   2. write the tail Deed at the NEW far end, and read it back
 *   3. write the head Deed
 *
 * The head is the commit point. Cut the power before it and the volume is
 * exactly what it was: the head still states the old length, and the new free
 * bits lie beyond the old data_blocks where nobody asks about them. Cut it
 * after, and the volume is grown, whole, with both copies agreeing. There is
 * no third state.
 *
 * ‼ ONE CORE. Growth is a write, and it runs inside a mount: the boot mount is
 * the BSP alone (arrivals are gated shut until TagFSBootMountSettled), and
 * every later mount is under attending_take(). No second core can be here.
 *
 * ‼ IT ONLY EVER GROWS. A volume that shrank would be a volume whose file
 * extents point outside it. The only comparison below is "more than it has".
 */
void volume_take_more_ground(uint8_t seat, uint64_t ground_sectors,
                                    DeedCopy *head)
{
    GroundV1Take t;
    VolumeLayout grown;
    uint64_t     old_tail = head->head.tail_sector;

    if (!GroundV1Measured(seat, ground_sectors, head, &t))
        return;

    /* --- 1. The new ground must be blank ------------------------------- */
    uint32_t set = 0;
    if (!ground_ahead_is_blank(t.data_now, (uint32_t)t.data_want, &set)) {
        if (set) {
            kprintf("[TagFS] seat %u: the bitmap already has %u block(s) marked "
                    "used past the end of this volume — that ground is not "
                    "blank and this volume does not take it\n", seat, set);
        } else {
            kprintf("[TagFS] seat %u: the bitmap past the end of this volume "
                    "would not read — nothing is taken\n", seat);
        }
        return;
    }

    if (!GroundV1DeedsWritten(seat, head, &t, &grown))
        return;

    /* Committed. Everything from here on is bookkeeping that costs a number if
     * it is interrupted, never a block. */
    memcpy(TagFSLayout(), &grown, sizeof(grown));
    tagfs_get_state()->block_end = grown.data_blocks;   /* the end of the ground a record may name */
    head->head.sectors     = t.new_sectors;
    head->head.tail_sector = t.new_tail;
    /* The Ledger's free count is NOT touched here, and that is the whole
     * reason growth needs no repair after an interruption: the count is
     * derived from the bitmap at the end of every mount (see "THE FREE COUNT
     * IS DERIVED"), so ground taken shows up in it by itself and ground taken
     * twice cannot be counted twice. It used to be added here and written, and
     * a growth that ran again after its head deed was rolled back left a
     * volume claiming more free blocks than it had blocks. */
    kprintf("[TagFS] seat %u: the volume now runs %llu sectors; its data run is "
            "%u blocks, %u more than it had, and its far copy has moved to "
            "sector %llu\n", seat, (unsigned long long)t.new_sectors,
            TagFSLayout()->data_blocks, (uint32_t)(t.data_want - t.data_now),
            (unsigned long long)t.new_tail);

    uint8_t *blank = (uint8_t *)kmalloc(DEED_PEN_BYTES);
    GroundGrowOldTailErased(seat, old_tail, blank);
    kfree(blank);
}
