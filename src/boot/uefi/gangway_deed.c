/*
 * Gangway — what a volume's Deed says.
 *
 * The Deed's block was read and checked while the device was being chosen
 * (gangway_medium.c, FindGroundOn: its magic, its two lengths, its checksum and its
 * role, in the kernel's order). What is left is to walk its stamps for the
 * answers this loader needs: which volume it is, where its book's data begins
 * (FRAME), and — when the Deed carries one — where the kernel is (BOOT).
 *
 * A Deed that bears no FRAME lays out no book: an older volume, or one that is
 * not TagFS's. This loader boots only a book, so such a volume is not a
 * refusal to stop at but a medium to pass over (gangway_medium.c, BlockIoProbeVolume).
 */

#include "gangway.h"

VolumeFrame  g_frame;
VolumeBoot   g_boot;
int          g_have_boot = 0;
uint8_t      g_volume_uuid[16];
uint64_t     g_volume_end = 0;
uint64_t     g_volume_atom_blocks = 1;

/* Walk the stamps for one kind. Returns a pointer into `raw`, or NULL. */
static const void *DeedStampOf(const uint8_t *raw, uint16_t kind, uint16_t *out_bytes)
{
    VolumeDeed deed;
    MemCopy(&deed, raw, sizeof(deed));

    uint32_t left = deed.stamp_bytes;
    const uint8_t *p = raw + deed.prologue_bytes;

    while (left >= sizeof(VolumeStamp)) {
        VolumeStamp st;
        MemCopy(&st, p, sizeof(st));
        if (st.bytes > left - sizeof(VolumeStamp)) return NULL;

        if (st.kind == kind) {
            if (out_bytes) *out_bytes = st.bytes;
            return p + sizeof(VolumeStamp);
        }
        uint32_t step = (sizeof(VolumeStamp) + st.bytes + 3U) & ~3U;
        if (step > left) return NULL;
        p += step; left -= step;
    }
    return NULL;
}

/*
 * The book, held to the rules every reader of one keeps, in the kernel's order
 * and words (deed.c): the block it is counted in first — a BOOT counted in
 * blocks of another size names other bytes — then the FRAME, by the one rule
 * (volume_deed.h, VolumeFrameCheck). A FRAME that holds and still has
 * something to say is said, as the kernel says it. Then BOOT has to name
 * blocks inside the data run, which runs to the far copy of the Deed; and the
 * volume's atom is kept, for the Quires of the ribbon its book is read along
 * begin on one (gangway_lectern_ribbon.c). A refusal is said with its line
 * left open, for DeedTakeUp's caller to end.
 */
static EFI_STATUS DeedTakeFrame(const uint8_t *raw, const VolumeDeed *deed,
                                const void *frame, uint16_t bytes)
{
    uint16_t geo_bytes = 0;
    const VolumeGeometry *geo =
        (const VolumeGeometry *)DeedStampOf(raw, VOLUME_STAMP_GEOMETRY, &geo_bytes);
    uint32_t block_bytes = (geo && geo_bytes >= sizeof(VolumeGeometry)) ? geo->block_bytes : 0;
    if (block_bytes != VOLUME_BLOCK_BYTES) {
        Print("Gangway: this volume was laid out in ");
        PrintDec(block_bytes);
        Print("-byte blocks and this loader speaks ");
        PrintDec(VOLUME_BLOCK_BYTES);
        return EFI_UNSUPPORTED;
    }

    const char *note = NULL;
    const char *why  = VolumeFrameCheck(frame, bytes, deed->tail_sector, &note);
    if (why) {
        Print("Gangway: this deed's FRAME does not hold: ");
        Print(why);
        return EFI_UNSUPPORTED;
    }
    if (note) {
        Print("Gangway: this deed's FRAME is ");
        PrintDec(bytes);
        Print(" bytes: ");
        Print(note);
        Print("\r\n");
    }
    MemCopy(&g_frame, frame, sizeof(g_frame));

    uint64_t run_end = deed->tail_sector / VOLUME_BLOCK_SECTORS;
    if (g_have_boot && (g_boot.kernel_block < g_frame.data_block ||
                        (uint64_t)g_boot.kernel_block + g_boot.kernel_blocks > run_end)) {
        Print("Gangway: this deed's BOOT names blocks outside its data run");
        return EFI_UNSUPPORTED;
    }
    g_volume_end         = run_end;
    g_volume_atom_blocks = BookAtomOf(geo->physical_bytes, 0, 0) / VOLUME_BLOCK_BYTES;
    return EFI_SUCCESS;
}

EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start)
{
    VolumeDeed deed;
    MemCopy(&deed, deed_block, sizeof(deed));
    MemCopy(g_volume_uuid, deed.uuid, 16);

    uint16_t frame_bytes = 0, bytes = 0;
    const void *frame = DeedStampOf(deed_block, VOLUME_STAMP_FRAME, &frame_bytes);
    if (!frame) {
        Print("Gangway: this deed bears no FRAME, so its volume is no book — an older one, or not TagFS's");
        return EFI_UNSUPPORTED;
    }

    /* A medium passed over before this one may have left its BOOT here. */
    g_have_boot = 0;
    const VolumeBoot *boot =
        (const VolumeBoot *)DeedStampOf(deed_block, VOLUME_STAMP_BOOT, &bytes);
    if (boot && bytes >= sizeof(VolumeBoot)) {
        MemCopy(&g_boot, boot, sizeof(g_boot));
        g_have_boot = (g_boot.kernel_block != 0 && g_boot.kernel_blocks != 0);
    }
    EFI_STATUS status = DeedTakeFrame(deed_block, &deed, frame, frame_bytes);
    if (EFI_ERROR(status)) return status;

    Print("Gangway: volume on ground at sector ");
    PrintDec(ground_start);
    Print(", a book with its data run from block ");
    PrintDec(g_frame.data_block);
    Print("\r\n");
    return EFI_SUCCESS;
}
