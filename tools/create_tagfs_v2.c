/*
 * create_tagfs_v2.c — format v2's part of the volume maker: --frame-only, a
 * format-v2 Deed over files in a row and no book. It is not a format-v2
 * writer — nothing writes a book yet (see below).
 */

#include "create_tagfs.h"

/* ====================================================================
 *  --frame-only: a format-v2 Deed over files in a row, and no book
 *
 * ‼ A STAND, NOT A FORMAT WRITER. Format v2 keeps what it knows about its
 * files in a book, and nothing writes a book yet. What this lays down is what
 * a kernel meets when a format-v2 volume is put in front of it before it can
 * read one: a Deed bearing FRAME instead of LAYOUT, the Spine ring's slots
 * empty, and the files in a row from the data run's first block with the
 * kernel first — so a loader boots it by BOOT exactly as it boots any volume.
 * That kernel must turn the volume away without writing a byte of it, and
 * this is how that is put to the test (tools/logcheck.sh framerefuse).
 *
 * The frame is derived from the grain the ground is aligned to, the way the
 * format lays one down: the Deed's grain, a grain to each Spine slot, and the
 * data run from the grain after the last of them.
 * ==================================================================== */

/* GEOMETRY · FRAME · BORN · BOOT · PACKED — and no LAYOUT, which is what says
 * this volume's book is not format v1. */
static int FrameOnlyDeed(FILE* disk, uint32_t role, uint64_t vsector,
                         const uint8_t uuid[16], const VolumeFrame* frame,
                         uint64_t volume_blocks, uint64_t made_at,
                         const VolumeBoot* boot, const VolumePacked* packed) {
    uint8_t        block[TAGFS_BLOCK_SIZE];
    VolumeGeometry geo = DeedGeometry();
    VolumeBorn     born;
    memset(&born, 0, sizeof(born));
    born.created_unix = made_at;
    memcpy(born.maker, "create_tagfs", 12);

    DeedPen pen;
    DeedPenOpen(&pen, block, sizeof(block));
    if (DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo,   sizeof(geo))     != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_FRAME,    frame,  sizeof(*frame))  != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BORN,     &born,  sizeof(born))    != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BOOT,     boot,   sizeof(*boot))   != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_PACKED,   packed, sizeof(*packed)) != 0) {
        fprintf(stderr, "The stamps of this Deed do not fit the block it lives in\n");
        return -1;
    }
    DeedPenSetUuid(&pen, uuid);
    DeedPenSetGround(&pen, volume_blocks * TAGFS_BLOCK_SECTORS,
                     (volume_blocks - 1) * TAGFS_BLOCK_SECTORS);
    DeedPenSetRole(&pen, role);
    DeedPenSeal(&pen);
    return write_at_sector(disk, vsector, block, sizeof(block));
}

/* Each file's size and blocks, and where it goes: from `from`, in a row, the
 * kernel first. The index of the kernel, or -1 when none is tagged; the first
 * block past the last file in *end. 0, or -1 having said why. */
static int FrameOnlyPlace(FileInfo* files, int file_count, char* argv[], int first,
                          uint64_t from, int* kernel, uint64_t* end) {
    seed_reserved_tags();
    *kernel = -1;
    for (int i = 0; i < file_count; i++) {
        FILE* f = fopen(argv[first + i * 2], "rb");
        if (!f) {
            fprintf(stderr, "Failed to open: %s\n", argv[first + i * 2]);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        files[i].filepath    = argv[first + i * 2];
        files[i].file_size   = (uint64_t)ftell(f);
        files[i].block_count = (uint32_t)((files[i].file_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
        if (files[i].block_count == 0) files[i].block_count = 1;
        fclose(f);

        const char* base = strrchr(files[i].filepath, '/');
        parse_tags(argv[first + i * 2 + 1], base ? base + 1 : files[i].filepath,
                   &files[i].tag_ids, &files[i].tag_count);
        if (KernelTagged(files[i].tag_ids, files[i].tag_count)) *kernel = i;
    }

    uint64_t next = from;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < file_count; i++) {
            if ((pass == 0) != (i == *kernel)) continue;
            if (next + files[i].block_count > UINT32_MAX) {
                fprintf(stderr, "The files run past block %u, the last a loader can be "
                                "pointed at\n", UINT32_MAX);
                return -1;
            }
            files[i].start_block = (uint32_t)next;
            next += files[i].block_count;
        }
    }
    *end = next;
    return 0;
}

int FrameOnlyMake(const char* disk_path, int file_count, char* argv[], int first) {
    FILE* disk = GroundOpen(disk_path);
    if (!disk) return 1;

    const uint32_t grain = GROUND_GRAIN_SECTORS / TAGFS_BLOCK_SECTORS;
    VolumeFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.spine_block  = grain;
    frame.spine_stride = grain;
    frame.spine_slots  = VOLUME_FRAME_SPINE_SLOTS;
    frame.data_block   = (uint64_t)grain * (1u + VOLUME_FRAME_SPINE_SLOTS);

    uint64_t  volume_blocks = g_ground_sectors / TAGFS_BLOCK_SECTORS;
    FileInfo* files  = calloc(file_count > 0 ? (size_t)file_count : 1, sizeof(FileInfo));
    int       kernel = -1;
    uint64_t  end    = 0;
    int       rc     = 1;
    if (!files) { fprintf(stderr, "calloc failed\n"); fclose(disk); return 1; }
    if (FrameOnlyPlace(files, file_count, argv, first, frame.data_block, &kernel, &end) != 0)
        goto out;
    if (volume_blocks < 2 || end > volume_blocks - 1) {
        fprintf(stderr, "The ground claimed for BoxOS is %llu blocks, and the files "
                        "laid from block %llu need it to run to block %llu and one "
                        "more for the far copy of the Deed\n",
                (unsigned long long)volume_blocks, (unsigned long long)frame.data_block,
                (unsigned long long)end);
        goto out;
    }

    /* The front matter, blank: the Deed's grain past the Deed and the Spine's
     * three, slots and all — a ring with no book in it reads as empty. */
    static const uint8_t blank[TAGFS_BLOCK_SIZE];
    for (uint64_t b = 1; b < frame.data_block; b++) {
        if (write_at_sector(disk, b * TAGFS_BLOCK_SECTORS, blank, sizeof(blank)) != 0) {
            fprintf(stderr, "Failed to blank block %llu\n", (unsigned long long)b);
            goto out;
        }
    }
    for (int i = 0; i < file_count; i++) {
        FILE* f = fopen(files[i].filepath, "rb");
        int wrote = f ? write_file_data(disk, 0, files[i].start_block, f,
                                        files[i].file_size, files[i].block_count) : -1;
        if (f) fclose(f);
        if (wrote != 0) { fprintf(stderr, "Failed to write %s\n", files[i].filepath); goto out; }
        printf("  File %2d: %-40s blocks %u-%u\n", i + 1, files[i].filepath,
               files[i].start_block, files[i].start_block + files[i].block_count - 1);
    }

    VolumeBoot boot;
    memset(&boot, 0, sizeof(boot));
    if (kernel >= 0) {
        boot.kernel_block  = files[kernel].start_block;
        boot.kernel_blocks = files[kernel].block_count;
        boot.kernel_bytes  = (uint32_t)files[kernel].file_size;
    } else {
        printf("  WARNING: nothing here is tagged 'kernel' — this volume carries "
               "no kernel and no loader will boot from it\n");
    }
    VolumePacked packed = PackedStamp(file_count, argv, first,
                                      kernel >= 0 ? files[kernel].filepath : NULL);

    uint8_t  uuid[16];
    uint64_t made_at = (uint64_t)time(NULL);
    uint64_t tail    = (volume_blocks - 1) * TAGFS_BLOCK_SECTORS;
    if (generate_uuid(uuid) != 0 ||
        FrameOnlyDeed(disk, VOLUME_DEED_ROLE_HEAD, 0, uuid, &frame, volume_blocks,
                      made_at, &boot, &packed) != 0 ||
        FrameOnlyDeed(disk, VOLUME_DEED_ROLE_TAIL, tail, uuid, &frame, volume_blocks,
                      made_at, &boot, &packed) != 0) {
        fprintf(stderr, "Failed to write the Deed\n");
        goto out;
    }
    printf("  Deed:          format v2 — FRAME, no LAYOUT; far copy at sector %llu\n"
           "  Spine ring:    %u slots %u blocks apart from block %llu, empty: no book\n"
           "  Data run:      from block %llu, the kernel first%s\n",
           (unsigned long long)tail, frame.spine_slots, frame.spine_stride,
           (unsigned long long)frame.spine_block, (unsigned long long)frame.data_block,
           kernel >= 0 ? "" : " (there is none)");
    rc = 0;
out:
    for (int i = 0; i < file_count; i++) free(files[i].tag_ids);
    free(files);
    /* Buffered writes: the close can be the first to say the medium did not
     * take them. */
    if (fclose(disk) != 0 && rc == 0) {
        fprintf(stderr, "Failed to finish writing the volume on %s\n", disk_path);
        rc = 1;
    }
    if (rc == 0)
        printf("\n[create_tagfs] The volume is made: format v2, its book not yet written.\n");
    return rc;
}
