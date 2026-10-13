/*
 * TagBoot — what a volume's Deed says.
 *
 * The Deed's block was read and checked while the device was being chosen
 * (tagboot.c, FindGroundOn: its magic, its two lengths, its checksum and its
 * role, in the kernel's order). What is left is to walk its stamps for the
 * answers this loader needs: which volume it is, which format its book is in,
 * where that book's data begins, and — when the Deed carries one — where the
 * kernel is.
 *
 * Which format a volume's book is in is said by the stamp its Deed bears:
 * LAYOUT for format v1, FRAME for format v2. The kernel refuses a Deed bearing
 * both (tagfs/deed/deed.c, DeedFormatRefused), and so does this loader: two
 * formats at once is a Deed nobody wrote, and to take either would be a guess.
 */

#include "tagboot.h"

VolumeLayout g_layout;
VolumeFrame  g_frame;
int          g_have_frame = 0;
VolumeBoot   g_boot;
int          g_have_boot = 0;
uint8_t      g_volume_uuid[16];

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

/* A format-v1 book: where LAYOUT says its data run is. */
static EFI_STATUS DeedTakeLayout(const VolumeDeed *deed, const void *layout, uint16_t bytes)
{
    if (!layout || bytes < sizeof(VolumeLayout)) {
        Print("TagBoot: this deed does not say where anything is\r\n");
        return EFI_UNSUPPORTED;
    }
    MemCopy(&g_layout, layout, sizeof(g_layout));

    uint64_t blocks_in_volume = deed->sectors / TAGFS_SECTORS_PER_BLOCK;
    if (g_layout.data_block == 0 || g_layout.data_blocks == 0 ||
        (uint64_t)g_layout.data_block + g_layout.data_blocks > blocks_in_volume) {
        Print("TagBoot: this deed lays out a data run that does not fit inside it\r\n");
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

/*
 * A format-v2 book, held to the rules every reader of one keeps, in the
 * kernel's order and words (deed.c, DeedFormatRefused): the block it is
 * counted in first — a BOOT counted in blocks of another size names other
 * bytes — then the FRAME, by the one rule (volume_deed.h, VolumeFrameCheck).
 * A FRAME that holds and still has something to say is said, as the kernel
 * says it. Then BOOT has to name blocks inside the data run, which runs to the
 * far copy of the Deed: the kernel's blocks are all this loader takes from a
 * format-v2 volume until it reads the book.
 */
static EFI_STATUS DeedTakeFrame(const uint8_t *raw, const VolumeDeed *deed,
                                const void *frame, uint16_t bytes)
{
    uint16_t geo_bytes = 0;
    const VolumeGeometry *geo =
        (const VolumeGeometry *)DeedStampOf(raw, VOLUME_STAMP_GEOMETRY, &geo_bytes);
    uint32_t block_bytes = (geo && geo_bytes >= sizeof(VolumeGeometry)) ? geo->block_bytes : 0;
    if (block_bytes != VOLUME_BLOCK_BYTES) {
        Print("TagBoot: this format-v2 volume was laid out in ");
        PrintDec(block_bytes);
        Print("-byte blocks and this loader speaks ");
        PrintDec(VOLUME_BLOCK_BYTES);
        Print("\r\n");
        return EFI_UNSUPPORTED;
    }

    const char *note = NULL;
    const char *why  = VolumeFrameCheck(frame, bytes, deed->tail_sector, &note);
    if (why) {
        Print("TagBoot: this deed's FRAME does not hold: ");
        Print(why);
        Print("\r\n");
        return EFI_UNSUPPORTED;
    }
    if (note) {
        Print("TagBoot: this deed's FRAME is ");
        PrintDec(bytes);
        Print(" bytes: ");
        Print(note);
        Print("\r\n");
    }
    MemCopy(&g_frame, frame, sizeof(g_frame));

    uint64_t run_end = deed->tail_sector / TAGFS_SECTORS_PER_BLOCK;
    if (g_have_boot && (g_boot.kernel_block < g_frame.data_block ||
                        (uint64_t)g_boot.kernel_block + g_boot.kernel_blocks > run_end)) {
        Print("TagBoot: this deed's BOOT names blocks outside its data run\r\n");
        return EFI_UNSUPPORTED;
    }
    g_have_frame = 1;
    return EFI_SUCCESS;
}

EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start)
{
    VolumeDeed deed;
    MemCopy(&deed, deed_block, sizeof(deed));
    MemCopy(g_volume_uuid, deed.uuid, 16);

    uint16_t layout_bytes = 0, frame_bytes = 0, bytes = 0;
    const void *layout = DeedStampOf(deed_block, VOLUME_STAMP_LAYOUT, &layout_bytes);
    const void *frame  = DeedStampOf(deed_block, VOLUME_STAMP_FRAME, &frame_bytes);
    if (layout && frame) {
        Print("TagBoot: this deed bears both LAYOUT and FRAME, its book in two formats at once\r\n");
        return EFI_UNSUPPORTED;
    }

    const VolumeBoot *boot =
        (const VolumeBoot *)DeedStampOf(deed_block, VOLUME_STAMP_BOOT, &bytes);
    if (boot && bytes >= sizeof(VolumeBoot)) {
        MemCopy(&g_boot, boot, sizeof(g_boot));
        g_have_boot = (g_boot.kernel_block != 0 && g_boot.kernel_blocks != 0);
    }
    EFI_STATUS status = frame ? DeedTakeFrame(deed_block, &deed, frame, frame_bytes)
                              : DeedTakeLayout(&deed, layout, layout_bytes);
    if (EFI_ERROR(status)) return status;

    Print("TagBoot: volume on ground at sector ");
    PrintDec(ground_start);
    if (g_have_frame) {
        Print(", a format-v2 book with its data run from block ");
        PrintDec(g_frame.data_block);
        Print("\r\n");
    } else {
        Print(", data run of ");
        PrintDec(g_layout.data_blocks);
        Print(" blocks\r\n");
    }
    return EFI_SUCCESS;
}
