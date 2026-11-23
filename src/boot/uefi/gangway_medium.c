/*
 * Gangway — the medium: every Block IO the firmware offers asked for the
 * ground its partition table claims for BoxOS and the Deed at the head of it,
 * the first that carries a book this loader boots kept, and the volume on it
 * read from then on, sector by sector, counted from its ground.
 */

#include "gangway.h"
#include "boxos_disks.h"

/* =========================================================================
 * Block IO: read sectors from disk
 * ========================================================================= */

static EFI_BLOCK_IO_PROTOCOL *g_block_io = NULL;

/* =========================================================================
 * The ground, and the Deed on it
 *
 * This loader used to read a superblock at absolute sector 1034 of whatever
 * device it was looking at. Now it reads the medium's partition table for the
 * run claimed for BoxOS, and the Deed at the head of that run — the same two
 * steps, over the same bytes, that the kernel takes when it mounts the volume
 * (core/boardroom/ground.c and tagfs/deed/deed.c).
 * ========================================================================= */

/* Where the volume being read begins on the medium, in 512-byte sectors. The
 * one absolute address this loader holds, and it is discovered, never assumed.
 * Every ReadSectors below is counted from it. */
static uint64_t g_ground_start = 0;

#define MBR_TABLE_OFFSET     446U
#define MBR_ENTRY_BYTES      16U
#define MBR_ENTRY_COUNT      4U
#define MBR_ENTRY_TYPE       4U
#define MBR_ENTRY_START_LBA  8U
#define MBR_ENTRY_SECTORS    12U
#define MBR_TYPE_BOXOS       0x7FU
#define MBR_TYPE_PROTECTIVE  0xEEU

#define GPT_HEADER_LBA          1U
#define GPT_HEADER_SIZE_OFFSET  0x0CU
#define GPT_HEADER_CRC_OFFSET   0x10U
#define GPT_ENTRY_LBA_OFFSET    0x48U
#define GPT_ENTRY_COUNT_OFFSET  0x50U
#define GPT_ENTRY_BYTES_OFFSET  0x54U
#define GPT_ENTRY_CRC_OFFSET    0x58U
#define GPT_ENTRY_TYPE_OFFSET   0U
#define GPT_ENTRY_FIRST_OFFSET  0x20U
#define GPT_ENTRY_LAST_OFFSET   0x28U
#define GPT_HEADER_MIN_BYTES    92U
#define GPT_ENTRY_MIN_BYTES     128U
#define GPT_ARRAY_MAX_BYTES     (64U * 1024U)

/* The type that marks BoxOS ground, as the kernel's DiskTable knows it. */
static const uint8_t g_boxos_type_guid[16] = DISK_TYPE_BOXOS;

static const uint8_t g_deed_magic[8] = {
    VOLUME_DEED_MAGIC_0, VOLUME_DEED_MAGIC_1, VOLUME_DEED_MAGIC_2,
    VOLUME_DEED_MAGIC_3, VOLUME_DEED_MAGIC_4, VOLUME_DEED_MAGIC_5,
    VOLUME_DEED_MAGIC_6, VOLUME_DEED_MAGIC_7
};


/* The block a device is addressed in, in bytes — 512, or 4096 on a 4Kn
 * medium — or 0 for one this loader cannot speak to: a block that is no whole
 * number of 512-byte sectors, or larger than 4096, as the kernel's room leaves
 * them (BOARDROOM_LOGICAL_MOST). */
static uint32_t MediumBlockBytes(const EFI_BLOCK_IO_PROTOCOL *bio)
{
    uint32_t bsz = bio && bio->media && bio->media->block_size ? bio->media->block_size : 512u;
    return bsz % 512u == 0 && bsz <= 4096u ? bsz : 0;
}

/* Whether `buffer` is one read_blocks takes as it is: on the medium's IoAlign
 * — 0 or 1 any, else a power of two (EFI_BLOCK_IO_MEDIA, UEFI 2.10 §13.9). */
static int MediumTakesBuffer(const EFI_BLOCK_IO_PROTOCOL *bio, const void *buffer)
{
    uint32_t align = bio->media->io_align;
    return align <= 1u || ((uintptr_t)buffer & (align - 1u)) == 0;
}

/*
 * Read 512-byte sectors straight off a device, with no volume in the picture —
 * the partition table, the probe that follows it, and the volume's own
 * blocks. Counted in 512-byte sectors whatever the device is addressed in; the
 * device is asked for whole blocks of its own only. The blocks the request
 * covers whole go straight into the caller's buffer in one call when the
 * buffer is on the medium's IoAlign; a block it covers only part of — at
 * either end, on a medium of blocks larger than a sector — and any block
 * whose buffer is off the alignment go through one of the loader's own, on
 * the 4096 any medium BoxOS meets asks of a buffer at the most.
 */
static EFI_STATUS ReadMediumSectors(EFI_BLOCK_IO_PROTOCOL *bio,
                                    uint64_t lba_512, uint32_t count_512,
                                    void *buffer)
{
    static uint8_t bounce[4096] __attribute__((aligned(4096)));
    if (!bio || !bio->media) return EFI_NOT_READY;
    uint32_t bsz = MediumBlockBytes(bio);
    if (bsz == 0) return EFI_UNSUPPORTED;

    const uint32_t per = bsz / 512u;
    uint8_t       *out = (uint8_t *)buffer;
    while (count_512 > 0) {
        uint64_t block = lba_512 / per;
        uint32_t head  = (uint32_t)(lba_512 % per);
        uint32_t whole = head ? 0 : count_512 / per;
        EFI_STATUS st;
        if (whole && MediumTakesBuffer(bio, out)) {
            st = bio->read_blocks(bio, bio->media->media_id, (EFI_LBA)block, (UINTN)whole * bsz, out);
            if (EFI_ERROR(st)) return st;
            out += (uint64_t)whole * bsz; lba_512 += (uint64_t)whole * per; count_512 -= whole * per;
            continue;
        }
        uint32_t take = per - head < count_512 ? per - head : count_512;
        st = bio->read_blocks(bio, bio->media->media_id, (EFI_LBA)block, bsz, bounce);
        if (EFI_ERROR(st)) return st;
        MemCopy(out, bounce + (uint64_t)head * 512u, (uint64_t)take * 512u);
        out += (uint64_t)take * 512u; lba_512 += take; count_512 -= take;
    }
    return EFI_SUCCESS;
}

/*
 * Is there a Deed at this sector, and is it the head of a volume?
 *
 * Same order of checks as the kernel's reader: magic, then the two lengths
 * bounded BEFORE anything is summed over them, then the checksum, then the
 * role. `out` gets the whole block so the stamps can be walked afterwards.
 */
static int DeedAt(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint32_t want_role,
                  uint8_t *out /* 4096 bytes */)
{
    if (EFI_ERROR(ReadMediumSectors(bio, lba, 8, out))) return 0;

    VolumeDeed deed;
    MemCopy(&deed, out, sizeof(deed));

    for (int i = 0; i < 8; i++)
        if (deed.magic[i] != g_deed_magic[i]) return 0;

    if (deed.prologue_bytes < sizeof(VolumeDeed) || deed.prologue_bytes > 4096)
        return 0;
    if (deed.stamp_bytes > 4096 - deed.prologue_bytes)
        return 0;

    uint32_t summed = (uint32_t)deed.prologue_bytes + deed.stamp_bytes;
    static uint8_t probe[4096];
    MemCopy(probe, out, summed);
    MemZero(probe + __builtin_offsetof(VolumeDeed, crc32), 4);
    if (Crc32(probe, summed) != deed.crc32) {
        Print("Gangway: a deed here does not match its own checksum\r\n");
        return 0;
    }

    if (deed.role != want_role) return 0;
    return 1;
}

/*
 * Find the run of this device claimed for BoxOS that carries a readable Deed.
 *
 * MBR and GPT both, because a medium BoxOS was installed onto by somebody
 * else's partitioning tool is the ordinary case on a real machine — and
 * because a GPT that does not check out must be refused rather than half-read.
 *
 * On success, sets the ground and leaves the Deed's block in `deed_out`.
 */
static int FindGroundOn(EFI_BLOCK_IO_PROTOCOL *bio, uint8_t *deed_out,
                        uint64_t *out_start, uint64_t *out_sectors)
{
    /* Every number of a table is in the medium's own blocks — 4096 bytes on
     * a 4Kn medium — and is multiplied into 512-byte sectors here. */
    const uint32_t bsz = MediumBlockBytes(bio);
    if (bsz == 0) return 0;
    const uint64_t ratio = bsz / 512u;
    static uint8_t sector0[512];
    if (EFI_ERROR(ReadMediumSectors(bio, 0, 1, sector0))) return 0;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA)     return 0;

    int protective = 0;
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES + MBR_ENTRY_TYPE] ==
            MBR_TYPE_PROTECTIVE) { protective = 1; break; }
    }

    if (!protective) {
        for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
            const uint8_t *e = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
            if (e[MBR_ENTRY_TYPE] != MBR_TYPE_BOXOS) continue;

            uint64_t start = Le32(e + MBR_ENTRY_START_LBA) * ratio;
            uint64_t count = Le32(e + MBR_ENTRY_SECTORS) * ratio;
            if (count < 8) continue;

            if (DeedAt(bio, start, VOLUME_DEED_ROLE_HEAD, deed_out)) {
                *out_start = start; *out_sectors = count; return 1;
            }
            /* The head is gone; the copy at the far end of the ground is the
             * volume's own second opinion, and it says the same things. */
            uint64_t tail = start + ((count / 8) - 1) * 8;
            if (DeedAt(bio, tail, VOLUME_DEED_ROLE_TAIL, deed_out)) {
                Print("Gangway: read the deed from the far end of the volume\r\n");
                *out_start = start; *out_sectors = count; return 1;
            }
        }
        return 0;
    }

    /* GPT. Both checksums, because a header that passes its own sum can still
     * point at an entry array that was interrupted mid-write. */
    static uint8_t header[4096];
    if (EFI_ERROR(ReadMediumSectors(bio, GPT_HEADER_LBA * ratio, (uint32_t)ratio, header))) return 0;

    const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++)
        if (header[i] != (uint8_t)sig[i]) return 0;

    uint32_t header_bytes = Le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES || header_bytes > bsz) return 0;

    static uint8_t hprobe[4096];
    MemCopy(hprobe, header, header_bytes);
    MemZero(hprobe + GPT_HEADER_CRC_OFFSET, 4);
    if (Crc32(hprobe, header_bytes) != Le32(header + GPT_HEADER_CRC_OFFSET)) {
        Print("Gangway: this disk's GPT header fails its own checksum\r\n");
        return 0;
    }

    uint64_t entry_lba   = Le64(header + GPT_ENTRY_LBA_OFFSET) * ratio;
    uint32_t entry_count = Le32(header + GPT_ENTRY_COUNT_OFFSET);
    uint32_t entry_bytes = Le32(header + GPT_ENTRY_BYTES_OFFSET);
    if (entry_bytes < GPT_ENTRY_MIN_BYTES || entry_count == 0 ||
        entry_bytes > GPT_ARRAY_MAX_BYTES ||
        entry_count > GPT_ARRAY_MAX_BYTES / entry_bytes) return 0;

    uint32_t array_bytes = entry_count * entry_bytes;
    static uint8_t array[GPT_ARRAY_MAX_BYTES];
    uint32_t array_secs = (array_bytes + 511) / 512;
    if (EFI_ERROR(ReadMediumSectors(bio, entry_lba, array_secs, array))) return 0;

    if (Crc32(array, array_bytes) != Le32(header + GPT_ENTRY_CRC_OFFSET)) {
        Print("Gangway: this disk's GPT entries fail their own checksum\r\n");
        return 0;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = array + (uint64_t)i * entry_bytes;
        int ours = 1;
        for (int b = 0; b < 16; b++)
            if (e[GPT_ENTRY_TYPE_OFFSET + b] != g_boxos_type_guid[b]) { ours = 0; break; }
        if (!ours) continue;

        uint64_t first = Le64(e + GPT_ENTRY_FIRST_OFFSET);
        uint64_t last  = Le64(e + GPT_ENTRY_LAST_OFFSET);
        if (last < first || (last - first + 1) * ratio < 8) continue;
        uint64_t count = (last - first + 1) * ratio;
        first *= ratio;

        if (DeedAt(bio, first, VOLUME_DEED_ROLE_HEAD, deed_out)) {
            *out_start = first; *out_sectors = count; return 1;
        }
        uint64_t tail = first + ((count / 8) - 1) * 8;
        if (DeedAt(bio, tail, VOLUME_DEED_ROLE_TAIL, deed_out)) {
            Print("Gangway: read the deed from the far end of the volume\r\n");
            *out_start = first; *out_sectors = count; return 1;
        }
    }
    return 0;
}

/* The block a medium's Deed is read into while the medium is probed;
 * DeedTakeUp copies out of it what the loader keeps. */
static uint8_t g_deed_block[4096];

/*
 * A medium in words a person can find it by: the device this loader came from
 * (`of` 0), or the one at `which` of the firmware's `of` Block IO handles —
 * and its size, and whether it comes out.
 */
static void SayMedium(EFI_BLOCK_IO_PROTOCOL *bio, UINTN which, UINTN of)
{
    if (of == 0) {
        Print("the device this loader came from");
    } else {
        Print("the medium at Block IO handle ");
        PrintDec(which + 1);
        Print(" of ");
        PrintDec(of);
    }
    Print(" (");
    PrintDec((((uint64_t)bio->media->last_block + 1) * bio->media->block_size) >> 20);
    Print(bio->media->removable_media ? " MiB, removable)" : " MiB, fixed)");
}

/*
 * Does this medium carry a book this loader boots? Its ground and Deed found
 * (FindGroundOn), the Deed taken up (DeedTakeUp). A volume that is no book —
 * an older volume, another system's data, a FRAME that does not hold — is said
 * in one line naming the medium and passed over: one such disk in a machine is
 * no reason not to boot the book on the next.
 */
static int BlockIoProbeVolume(EFI_BLOCK_IO_PROTOCOL *bio, UINTN which, UINTN of)
{
    if (!bio || !bio->media) return 0;
    if (bio->media->block_size == 0 || !bio->media->media_present) return 0;

    uint64_t start = 0, sectors = 0;
    if (!FindGroundOn(bio, g_deed_block, &start, &sectors)) return 0;
    if (EFI_ERROR(DeedTakeUp(g_deed_block, start))) {
        Print("; passed over: ");
        SayMedium(bio, which, of);
        Print("\r\n");
        return 0;
    }

    g_ground_start = start;
    return 1;
}
/* The medium chosen, and the size of its blocks. */
static void SayDiskFound(void)
{
    Print("Gangway: disk found, block_size=");
    PrintDec(g_block_io->media->block_size);
    Print("\r\n");
}
/*
 * Find the BlockIO handle whose volume is a book this loader boots.
 *
 * Strategy:
 *   1. Try the loaded-image device first (common case: booting from same disk
 *      that holds both ESP and TagFS, e.g. on real hardware with GPT layout).
 *   2. Scan every non-partition BlockIO handle, reading each one's partition
 *      table for BoxOS ground and each ground for a Deed (handles the QEMU
 *      two-drive setup and GPT/MBR alike).
 * A medium whose volume is no book is passed over, said, and the scan goes on
 * (BlockIoProbeVolume).
 *
 * The probe is safe: it reads a partition table and, at most, one 4 KiB block
 * from a disk that may have nothing of ours on it, and writes nothing.
 */
EFI_STATUS BlockIoFindDevice(EFI_HANDLE image_handle)
{
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_io_guid     = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_HANDLE probed = NULL;
    EFI_STATUS status;

    Print("Gangway: scanning for TagFS block device...\r\n");

    status = g_bs->handle_protocol(image_handle,
                                   &loaded_image_guid,
                                   (void **)&loaded_image);
    if (!EFI_ERROR(status) && loaded_image) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        probed = loaded_image->device_handle;
        status = g_bs->handle_protocol(loaded_image->device_handle,
                                       &block_io_guid,
                                       (void **)&bio);
        if (!EFI_ERROR(status) && BlockIoProbeVolume(bio, 0, 0)) {
            Print("Gangway: the volume is on the device this loader came from\r\n");
            g_block_io = bio;
            SayDiskFound();
            return EFI_SUCCESS;
        }
    }

    /* The loaded-image device carries no book.
     * Enumerate all raw (non-partition) BlockIO handles and probe each. */
    Print("Gangway: loaded-image device has no TagFS book, scanning all handles...\r\n");

    UINTN       handle_count = 0;
    EFI_HANDLE *handles      = NULL;
    status = g_bs->locate_handle_buffer(EFI_BY_PROTOCOL,
                                        &block_io_guid,
                                        NULL,
                                        &handle_count,
                                        &handles);
    if (EFI_ERROR(status)) return status;

    EFI_STATUS found = EFI_NOT_FOUND;

    for (UINTN i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        /* Probed already, and said if it was passed over: once is enough. */
        if (handles[i] == probed) continue;
        if (EFI_ERROR(g_bs->handle_protocol(handles[i], &block_io_guid, (void **)&bio)))
            continue;
        if (!bio || !bio->media)              continue;
        if (bio->media->logical_partition)    continue;  /* skip partitions */

        /* Hot-plugged removable media (USB sticks, SD cards) frequently
         * report media_present=0 on the first probe — the controller
         * hasn't latched the media-change yet. UEFI spec §13.9: issue a
         * Reset(verify=FALSE), then re-check. If still absent, skip. */
        if (!bio->media->media_present) {
            if (bio->reset) {
                (void)bio->reset(bio, FALSE);
            }
            if (!bio->media->media_present) continue;
        }

        if (BlockIoProbeVolume(bio, i, handle_count)) {
            g_block_io = bio;
            found = EFI_SUCCESS;
            break;
        }
    }

    g_bs->free_pool(handles);

    if (EFI_ERROR(found))
        Print("Gangway: no medium here carries a book — a TagFS volume whose deed this loader reads and holds\r\n");
    else
        SayDiskFound();

    return found;
}

/* Read 512-byte logical sectors of the VOLUME — counted from the start of its
 * ground, never from the start of the medium. The one place the base is
 * applied, which is what makes "nothing inside a volume is an absolute
 * address" hold here as well as in the kernel. */
EFI_STATUS ReadSectors(uint64_t vlba, uint32_t count_512, void *buffer)
{
    if (!g_block_io) return EFI_NOT_READY;
    return ReadMediumSectors(g_block_io, g_ground_start + vlba, count_512, buffer);
}

EFI_STATUS MediumRead(uint64_t lba_512, uint32_t count_512, void *buffer)
{
    if (!g_block_io) return EFI_NOT_READY;
    return ReadMediumSectors(g_block_io, lba_512, count_512, buffer);
}

/* Through a buffer of its own, on the 4096 any IoAlign a medium of these
 * blocks asks for is at the most (§13.9); then flushed, so what was written is
 * on the medium and not in its cache when the kernel is jumped into. A
 * medium that has no cache to flush says EFI_UNSUPPORTED, which is no
 * refusal. */
EFI_STATUS MediumWriteSlot(uint64_t lba_512, const void *slot)
{
    static uint8_t aligned[4096] __attribute__((aligned(4096)));

    if (!g_block_io || !g_block_io->media) return EFI_NOT_READY;
    if (g_block_io->media->read_only) return EFI_WRITE_PROTECTED;
    uint32_t bsz = MediumBlockBytes(g_block_io);
    if (bsz == 0 || (lba_512 * 512u) % bsz) return EFI_UNSUPPORTED;

    MemCopy(aligned, slot, sizeof(aligned));
    EFI_STATUS st = g_block_io->write_blocks(g_block_io, g_block_io->media->media_id,
                                             (EFI_LBA)(lba_512 * 512u / bsz), sizeof(aligned), aligned);
    if (EFI_ERROR(st)) return st;
    st = g_block_io->flush_blocks(g_block_io);
    return st == EFI_UNSUPPORTED ? EFI_SUCCESS : st;
}

/* The medium's MBR read, and every partition it names begins at `end` or past
 * it: 1. A GPT's protective entry is a table this loader does not answer for
 * the front of — its entries lie at sectors 2 to 33 — and is 0. */
int MediumFrontFree(uint64_t end)
{
    static uint8_t mbr[512] __attribute__((aligned(4096)));

    const uint32_t bsz = g_block_io ? MediumBlockBytes(g_block_io) : 0;
    if (bsz == 0 || EFI_ERROR(MediumRead(0, 1, mbr)) || mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = mbr + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        if (e[MBR_ENTRY_TYPE] == 0) continue;
        if (e[MBR_ENTRY_TYPE] == MBR_TYPE_PROTECTIVE ||
            (uint64_t)Le32(e + MBR_ENTRY_START_LBA) * (bsz / 512u) < end) return 0;
    }
    return 1;
}
