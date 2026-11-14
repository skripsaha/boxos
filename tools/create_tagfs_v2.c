/*
 * create_tagfs_v2.c — format v2's part of the volume maker: --v2, a format-v2
 * volume, its files in a row and its book bound over them (the binder is
 * create_tagfs_bind.c); --ground --v2, how far its ground must run; and
 * --frame-only, a format-v2 Deed over files in a row and no book — a stand.
 */

#include <unistd.h>           /* fsync: the order the medium takes the writes in */

#include "create_tagfs_bind.h"
#include "volume_digest.h"   /* the digest of a block, bit for bit the kernel's */

/* The last tag id the kernel gives a volume (tagfs_constants.h, which a host
 * tool cannot include): bit 31 is its Logbook's (spec_v2 §6.4). Format v2
 * writes an id as a varint and has no width of its own, but a volume naming
 * an id past this one is a volume the kernel may only read (tally.py
 * TagGlass) — so the cargo interns none past it. */
#define TAGFS_MAX_TAG_ID  0x7FFFFFFEu

/* ====================================================================
 *  --frame-only: a format-v2 Deed over files in a row, and no book
 *
 * ‼ A STAND, NOT A FORMAT WRITER. Format v2 keeps what it knows about its
 * files in a book, and this writes none (--v2, below, does). What it lays
 * down is what a kernel meets when a format-v2 volume with no book is put in
 * front of it: a Deed bearing FRAME instead of LAYOUT, the Spine ring's slots
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
 * kernel first. The index of the kernel, or -1 when none is given the role; the first
 * block past the last file in *end. 0, or -1 having said why. */
static int FrameOnlyPlace(FileInfo* files, int file_count, char* argv[], int first,
                          uint64_t from, int* kernel, uint64_t* end) {
    seed_reserved_tags(TAGFS_MAX_TAG_ID);
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
        if (parse_tags(argv[first + i * 2 + 1], base ? base + 1 : files[i].filepath,
                       &files[i].tag_ids, &files[i].tag_count, &files[i].role) != 0)
            return -1;
        if (files[i].role == TALLY_FILE_ROLE_KERNEL) *kernel = i;
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

/* The FRAME every format-v2 volume this tool makes is laid out by: the Deed's
 * grain, a grain to each Spine slot, the data run from the grain after. */
static VolumeFrame V2Frame(void) {
    const uint32_t grain = GROUND_GRAIN_SECTORS / TAGFS_BLOCK_SECTORS;
    VolumeFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.spine_block  = grain;
    frame.spine_stride = grain;
    frame.spine_slots  = VOLUME_FRAME_SPINE_SLOTS;
    frame.data_block   = (uint64_t)grain * (1u + VOLUME_FRAME_SPINE_SLOTS);
    return frame;
}

int FrameOnlyMake(const char* disk_path, int file_count, char* argv[], int first) {
    FILE* disk = GroundOpen(disk_path);
    if (!disk) return 1;

    VolumeFrame frame = V2Frame();
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
        printf("  WARNING: nothing here is given the role @kernel — this volume carries "
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

/* ====================================================================
 *  --v2: a format-v2 volume, its book bound
 *
 * The ground, the Deed and the row of files --frame-only lays, and the book a
 * kernel stands on (spec_v2 §9.3, §16.2 п.23, §16.3 п.5): the Tally of seq 1
 * — every file's header and Parcels, every tag, the digest of every block of
 * the files — in both its Tally-Quires, slot 0 of the Spine at seq 2 naming
 * it, and the ribbon's Quire and its next_quire blank. Nothing a running
 * kernel writes is written here: no stitch, and no Slip — a file's last block
 * is a block (§9.3); an empty file is a header alone (§2.9).
 *
 * ‼ NOTHING IS WRITTEN UNTIL IT IS KNOWN TO FIT. The files are placed, the
 * volume's identity drawn and the digest of every block taken — a digest is
 * seeded by the uuid (volume_digest.h), so the uuid comes first — and the book
 * laid out in memory; only then is the first byte written. A ground the book
 * does not fit is refused with not a byte of it touched. The data is read a
 * second time to be written, and every block written is held to the digest
 * the book carries for it: a file that changed between the two is refused.
 *
 * ‼ u64 THROUGH. A format-v2 volume counts its blocks in varints (§2.4) and is
 * not held to 16 TiB; the kernel's own run alone is held to the Deed's BOOT,
 * whose fields are u32.
 * ==================================================================== */

#define V2_SEQ_TALLY  1u   /* the first Tally's seq — and every run's birth, every file's gen (§16.2 п.23) */
#define V2_SEQ_SLOT   2u   /* slot 0's */

typedef struct {
    FILE*       disk;          /* NULL for --ground: nothing is written */
    VolumeFrame frame;
    uint64_t    volume_blocks;
    BindFile*   files;
    int         count;
    int         kernel;        /* the file in the kernel's role, placed first, or -1 */
    uint64_t    end;           /* the first block past the files */
    uint64_t*   digests;       /* a block's, from frame.data_block to `end` */
    uint8_t     uuid[16];
    uint64_t    lane;          /* the digests' seed, lane 0 of the uuid (BoxHashIntegrity) */
    uint64_t    made_at;       /* seconds: BORN, and every file's times in ns */
    BindLeaves  leaves;
    BindPlan    plan;
} V2Bind;

static int V2IdOrder(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/* One file's size and tags — rising, as the book writes them (§2.9) — and
 * the role it is kept in. 0, or -1 having said why. */
static int V2FileOf(BindFile* f, uint64_t fid, const char* path, const char* tags) {
    uint32_t* ids = NULL;
    uint32_t  n   = 0;
    FILE*     src = fopen(path, "rb");
    if (!src) {
        fprintf(stderr, "Failed to open: %s\n", path);
        return -1;
    }
    fseek(src, 0, SEEK_END);
    long size = ftell(src);
    fclose(src);
    if (size < 0) {
        fprintf(stderr, "Cannot tell how long %s is\n", path);
        return -1;
    }
    const char* base = strrchr(path, '/');
    f->path   = path;
    f->name   = base ? base + 1 : path;
    f->fid    = fid;
    f->size   = (uint64_t)size;
    f->blocks = (f->size + TAGFS_BLOCK_SIZE - 1u) / TAGFS_BLOCK_SIZE;
    if (parse_tags(tags, f->name, &ids, &n, &f->role) != 0) return -1;
    qsort(ids, n, sizeof(uint32_t), V2IdOrder);
    f->tags  = ids;
    f->ntags = n;
    return 0;
}

/* Every file, fid by the order given, and its place: from the data run's
 * first block, in a row, the kernel first — as --frame-only places them, but
 * an empty file takes no block. 0, or -1 having said why. */
static int V2Place(V2Bind* v, char* argv[], int first) {
    seed_reserved_tags(TAGFS_MAX_TAG_ID);
    v->kernel = -1;
    for (int i = 0; i < v->count; i++) {
        if (V2FileOf(&v->files[i], (uint64_t)i + 1u, argv[first + i * 2], argv[first + i * 2 + 1]) != 0)
            return -1;
        if (v->files[i].role == TALLY_FILE_ROLE_KERNEL) v->kernel = i;
    }
    uint64_t next = v->frame.data_block;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < v->count; i++) {
            if ((pass == 0) != (i == v->kernel)) continue;
            v->files[i].start = next;
            next += v->files[i].blocks;
        }
    }
    v->end = next;
    if (v->kernel >= 0) {
        const BindFile* k = &v->files[v->kernel];
        if (k->start + k->blocks > UINT32_MAX || k->size > UINT32_MAX) {
            fprintf(stderr, "The kernel, %s, would run to block %llu over %llu bytes: the Deed's BOOT, which a "
                            "loader is pointed at, counts both in 32 bits\n", k->path,
                    (unsigned long long)(k->start + k->blocks), (unsigned long long)k->size);
            return -1;
        }
    }
    return 0;
}

/* A file's blocks, read from its source and each one's digest taken (lane 0
 * of the uuid, BoxHashIntegrity bit for bit; the last block's tail zeros, as
 * written): into `digests` when `disk` is NULL; else each block written to
 * its place, and held to the digest already taken. 0, or -1 having said why. */
static int V2FileBlocks(FILE* disk, const BindFile* f, uint64_t lane, uint64_t* digests) {
    static uint8_t block[TAGFS_BLOCK_SIZE];
    uint64_t       left = f->size;
    FILE*          src  = fopen(f->path, "rb");
    if (!src) {
        fprintf(stderr, "Failed to open: %s\n", f->path);
        return -1;
    }
    for (uint64_t i = 0; i < f->blocks; i++) {
        size_t want = left > TAGFS_BLOCK_SIZE ? TAGFS_BLOCK_SIZE : (size_t)left;
        memset(block, 0, sizeof(block));
        if (fread(block, 1, want, src) != want) {
            fprintf(stderr, "Short read of %s at its block %llu\n", f->path, (unsigned long long)i);
            fclose(src);
            return -1;
        }
        left -= want;
        uint64_t digest = WyHash64(block, TAGFS_BLOCK_SIZE, lane);
        if (!disk) {
            digests[i] = digest;
        } else if (digest != digests[i]) {
            fprintf(stderr, "%s changed while the volume was being made: its block %llu is not the block "
                            "its digest was taken of\n", f->path, (unsigned long long)i);
            fclose(src);
            return -1;
        } else if (write_at_sector(disk, (f->start + i) * TAGFS_BLOCK_SECTORS, block, sizeof(block)) != 0) {
            fprintf(stderr, "Failed to write %s at block %llu\n", f->path, (unsigned long long)(f->start + i));
            fclose(src);
            return -1;
        }
    }
    fclose(src);
    return 0;
}

/* Room for a digest of every block of the files — and, with `read`, each
 * one taken from the files themselves. 0, or -1 having said why. */
static int V2Digests(V2Bind* v, int read) {
    v->digests = calloc(v->end > v->frame.data_block ? v->end - v->frame.data_block : 1u, sizeof(uint64_t));
    if (!v->digests) {
        fprintf(stderr, "No memory for the digests of %llu blocks\n",
                (unsigned long long)(v->end - v->frame.data_block));
        return -1;
    }
    for (int i = 0; read && i < v->count; i++) {
        const BindFile* f = &v->files[i];
        if (f->blocks && V2FileBlocks(NULL, f, v->lane, v->digests + (f->start - v->frame.data_block)) != 0)
            return -1;
    }
    return 0;
}

/* FILES: each file's header — gen 1, ACTIVE, its size, made and modified at
 * BORN, its name and tags — and its one run as its Parcels (§2.9). */
static int V2FileLeaves(V2Bind* v) {
    BindLeaves* l  = &v->leaves;
    uint64_t    ns = v->made_at * 1000000000ull;
    for (int i = 0; i < v->count; i++) {
        const BindFile* f    = &v->files[i];
        uint8_t*        ids  = BindBody(l, 10u * (uint64_t)f->ntags);
        uint8_t*        body = BindBody(l, 80u + strlen(f->name) + 10u * (uint64_t)f->ntags);
        if (!ids || !body) return -1;
        TallyFileHead h = { .gen = V2_SEQ_TALLY, .flags = TALLY_FILE_ACTIVE | f->role, .size = f->size,
                            .created_ns = ns, .modified_ns = ns, .name = (const uint8_t*)f->name,
                            .name_len = strlen(f->name), .ntags = f->ntags, .tags = ids };
        h.tags_bytes = TallyIdsPut(ids, f->tags, f->ntags);
        if (BindAdd(l, TALLY_FILES, (TallyKey){ f->fid, 0 }, body, TallyFileHeadPut(body, &h), 1) != 0)
            return -1;
        TallyRun run = { .start = f->start, .count = f->blocks, .birth = V2_SEQ_TALLY };
        if (BindParcels(l, f->fid, &run, f->blocks ? 1u : 0u) != 0) return -1;
    }
    return 0;
}

/* TAGS: every tag interned — the reserved vocabulary at its ids, and every
 * one a file wears — its key, and its value when it has one. */
static int V2TagLeaves(V2Bind* v) {
    BindLeaves* l = &v->leaves;
    for (uint32_t id = 0; id < g_tag_count; id++) {
        const TagEntry* e = &g_tags[id];
        TallyTag t = { .flags = e->value ? TALLY_TAG_HAS_VALUE : 0u, .key = (const uint8_t*)e->key,
                       .key_len = strlen(e->key), .value = (const uint8_t*)e->value,
                       .value_len = e->value ? strlen(e->value) : 0u };
        uint8_t* body = BindBody(l, 30u + t.key_len + t.value_len);
        if (!body || BindAdd(l, TALLY_TAGS, (TallyKey){ id, 0 }, body, TallyTagPut(body, &t), 1) != 0)
            return -1;
    }
    return 0;
}

/* DIGESTS: the files' blocks are one row from the data run's first block, cut
 * at every window of DIGEST_ITEM_MOST blocks (§16.3 п.1). */
static int V2DigestLeaves(V2Bind* v) {
    BindLeaves* l = &v->leaves;
    for (uint64_t b = v->frame.data_block; b < v->end;) {
        uint64_t stop = (b / DIGEST_ITEM_MOST + 1u) * DIGEST_ITEM_MOST;
        uint32_t n    = (uint32_t)((stop < v->end ? stop : v->end) - b);
        uint8_t* body = BindBody(l, 10u + 8u * (uint64_t)n);
        if (!body) return -1;
        uint32_t len = TallyDigestsPut(body, v->digests + (b - v->frame.data_block), n);
        if (BindAdd(l, TALLY_DIGESTS, (TallyKey){ b, 0 }, body, len, 1) != 0) return -1;
        b += n;
    }
    return 0;
}

/* Every leaf element of the Tally, and the ground's grain and atom. USE is
 * not among them: a new volume remembers no Use Context, and an empty tree
 * is not written (§1.4). 0, or -1 having said why. */
static int V2Leaves(V2Bind* v) {
    VolumeGeometry geo = DeedGeometry();
    v->leaves.grain = geo.grain_bytes / TAGFS_BLOCK_SIZE;
    v->leaves.grid  = geo.physical_bytes > TAGFS_BLOCK_SIZE ? geo.physical_bytes / TAGFS_BLOCK_SIZE : 1u;
    v->leaves.seq   = V2_SEQ_TALLY;
    if (V2FileLeaves(v) != 0 || V2TagLeaves(v) != 0 || V2DigestLeaves(v) != 0) {
        fprintf(stderr, "No memory for the elements of the Tally\n");
        return -1;
    }
    return 0;
}

/* Everything written so far handed to the medium and on it before anything
 * after it is: the order a volume cut short is read in. 0, or -1 having said
 * why. */
static int V2Settle(FILE* disk, const char* what) {
    if (fflush(disk) == 0 && fsync(fileno(disk)) == 0) return 0;
    fprintf(stderr, "The medium did not take %s\n", what);
    return -1;
}

/* The Deed's block at both ends blanked and on the medium before anything
 * else is written — a ground that carried a volume keeps that volume's Deed
 * until then, and a Deed left standing through the writes below would name a
 * volume they overwrote. Then the front blank — the Deed's grain past the
 * Deed, and the Spine's three — and the files' blocks, each held to its
 * digest. 0, or -1. */
static int V2WriteData(V2Bind* v) {
    static const uint8_t blank[TAGFS_BLOCK_SIZE];
    uint64_t tail = (v->volume_blocks - 1u) * TAGFS_BLOCK_SECTORS;
    if (write_at_sector(v->disk, 0, blank, sizeof(blank)) != 0 ||
        write_at_sector(v->disk, tail, blank, sizeof(blank)) != 0) {
        fprintf(stderr, "Failed to blank the Deed's blocks\n");
        return -1;
    }
    if (V2Settle(v->disk, "the Deed's blocks blanked") != 0) return -1;
    for (uint64_t b = 1; b < v->frame.data_block; b++) {
        if (write_at_sector(v->disk, b * TAGFS_BLOCK_SECTORS, blank, sizeof(blank)) != 0) {
            fprintf(stderr, "Failed to blank block %llu\n", (unsigned long long)b);
            return -1;
        }
    }
    for (int i = 0; i < v->count; i++) {
        const BindFile* f = &v->files[i];
        if (f->blocks &&
            V2FileBlocks(v->disk, f, v->lane, v->digests + (f->start - v->frame.data_block)) != 0)
            return -1;
        if (f->blocks)
            printf("  File %2d: %-40s blocks %llu-%llu\n", i + 1, f->path, (unsigned long long)f->start,
                   (unsigned long long)(f->start + f->blocks - 1u));
        else
            printf("  File %2d: %-40s empty: a header alone\n", i + 1, f->path);
    }
    return 0;
}

/* The book — the Tally's nodes in both copies, then slot 0 naming them — on
 * the medium with the files, and only then the Deed at both ends, on the
 * medium in its turn: a volume cut short anywhere before has no Deed at all
 * (V2WriteData blanked the one a volume before it had), never one that names
 * a book half written. 0, or -1. */
static int V2WriteBook(V2Bind* v, char* argv[], int first) {
    const SpineHead   head   = { V2_SEQ_TALLY, 0, 0, v->made_at * 1000000000ull };
    const SpineCounts counts = { (uint64_t)v->count + 1u, g_tag_count, 1 };
    VolumeBoot        boot;
    memset(&boot, 0, sizeof(boot));
    if (BindTally(v->disk, &v->plan, v->uuid, V2_SEQ_TALLY) != 0 ||
        BindSpine(v->disk, &v->plan, v->frame.spine_block, v->leaves.grain, v->uuid, &head, &counts,
                  V2_SEQ_SLOT) != 0)
        return -1;
    if (v->kernel >= 0) {
        boot.kernel_block  = (uint32_t)v->files[v->kernel].start;
        boot.kernel_blocks = (uint32_t)v->files[v->kernel].blocks;
        boot.kernel_bytes  = (uint32_t)v->files[v->kernel].size;
    } else {
        printf("  WARNING: nothing here is given the role @kernel — this volume carries "
               "no kernel and no loader will boot from it\n");
    }
    VolumePacked packed = PackedStamp(v->count, argv, first,
                                      v->kernel >= 0 ? v->files[v->kernel].path : NULL);
    uint64_t tail = (v->volume_blocks - 1u) * TAGFS_BLOCK_SECTORS;
    if (V2Settle(v->disk, "the volume's files and book before its Deed") != 0) return -1;
    if (FrameOnlyDeed(v->disk, VOLUME_DEED_ROLE_HEAD, 0, v->uuid, &v->frame, v->volume_blocks, v->made_at,
                      &boot, &packed) != 0 ||
        FrameOnlyDeed(v->disk, VOLUME_DEED_ROLE_TAIL, tail, v->uuid, &v->frame, v->volume_blocks, v->made_at,
                      &boot, &packed) != 0) {
        fprintf(stderr, "Failed to write the Deed\n");
        return -1;
    }
    if (V2Settle(v->disk, "the volume's Deed") != 0) return -1;
    printf("  Deed:          format v2 — FRAME, no LAYOUT; far copy at sector %llu\n"
           "  Tally:         seq %u, %u node(s) twice — Tally-Quires at blocks %llu and %llu, %llu grain(s) each\n"
           "  Spine:         slot 0 at block %llu, seq %u; slots 1 and 2 blank\n"
           "  Ribbon:        its Quire at block %llu and its next_quire at %llu, blank\n",
           (unsigned long long)tail, V2_SEQ_TALLY, v->plan.nnodes, (unsigned long long)v->plan.a,
           (unsigned long long)v->plan.b, (unsigned long long)v->plan.k,
           (unsigned long long)v->frame.spine_block, V2_SEQ_SLOT, (unsigned long long)v->plan.ribbon,
           (unsigned long long)v->plan.next);
    return 0;
}

static void V2Release(V2Bind* v) {
    for (int i = 0; v->files && i < v->count; i++) free(v->files[i].tags);
    free(v->files);
    free(v->digests);
    BindLeavesFree(&v->leaves);
    BindPlanFree(&v->plan);
    CargoRelease();
}

/* The files and the book laid out, and the book refused when the ground
 * cannot hold it. `data_end` is the far Deed's block (UINT64_MAX: no ground
 * yet, --ground asks how much). 0, or -1 having said why. */
static int V2Lay(V2Bind* v, char* argv[], int first, uint64_t data_end, int read) {
    v->files = calloc(v->count > 0 ? (size_t)v->count : 1u, sizeof(BindFile));
    if (!v->files) {
        fprintf(stderr, "calloc failed\n");
        return -1;
    }
    v->frame   = V2Frame();
    v->made_at = (uint64_t)time(NULL);
    if (V2Place(v, argv, first) != 0) return -1;
    if (v->end > data_end) {
        fprintf(stderr, "The ground claimed for BoxOS is %llu blocks, and the files laid from block %llu "
                        "need it to run to block %llu and one more for the far copy of the Deed\n",
                (unsigned long long)v->volume_blocks, (unsigned long long)v->frame.data_block,
                (unsigned long long)v->end);
        return -1;
    }
    if (read) {
        uint64_t lanes[4];
        if (generate_uuid(v->uuid) != 0) return -1;
        WySeedLanes(v->uuid, sizeof(v->uuid), lanes);
        v->lane = lanes[0];
    }
    return V2Digests(v, read) == 0 && V2Leaves(v) == 0 && QuireLay(&v->leaves, v->end, data_end, &v->plan) == 0
           ? 0 : -1;
}

int V2BindMake(const char* disk_path, int file_count, char* argv[], int first) {
    V2Bind v;
    memset(&v, 0, sizeof(v));
    v.count = file_count;
    v.disk  = GroundOpen(disk_path);
    if (!v.disk) return 1;
    v.volume_blocks = g_ground_sectors / TAGFS_BLOCK_SECTORS;
    int made = v.volume_blocks >= 2u && V2Lay(&v, argv, first, v.volume_blocks - 1u, 1) == 0 &&
               V2WriteData(&v) == 0 && V2WriteBook(&v, argv, first) == 0;
    if (v.volume_blocks < 2u)
        fprintf(stderr, "The ground claimed for BoxOS is %llu block(s): no room for a Deed at both ends\n",
                (unsigned long long)v.volume_blocks);
    /* Buffered writes: the close can be the first to say the medium did not
     * take them. */
    if (fclose(v.disk) != 0 && made) {
        fprintf(stderr, "Failed to finish writing the volume on %s\n", disk_path);
        made = 0;
    }
    if (made)
        printf("\n[create_tagfs] The volume is made: format v2, %d file(s), its book bound.\n", file_count);
    V2Release(&v);
    return made ? 0 : 1;
}

/* ====================================================================
 *  --ground --v2: how far a format-v2 volume's ground must run
 *
 * The answer the Makefile makes the medium give (§16.2 п.23): the front four
 * grains, the files, the two Tally-Quires of k grains — the k the book of
 * these very files lays, laid here by the binder that will lay it — the
 * ribbon's Quire and its next_quire, and R past them: the reserve the
 * kernel's allocator keeps so that a turn of the ribbon and one whole Tally
 * are always possible, one ribbon Quire and two Tally-Quires' worth
 * (§16.2 п.6) — the room a running machine works in past that, as a volume of
 * format v1 is given it (GROUND_WORKING_ROOM_BLOCKS), so a book takes the
 * writes of the same machine — then the block of the far Deed, all of it to
 * the grain.
 *
 * The digests' values are not known before the volume has a uuid and do not
 * change how much room they take; the times are seconds of today in either
 * call, as long in varints.
 * ==================================================================== */

int V2GroundSay(int file_count, char* argv[], int first) {
    V2Bind v;
    memset(&v, 0, sizeof(v));
    v.count = file_count;
    int said = V2Lay(&v, argv, first, UINT64_MAX, 0) == 0;
    if (said) {
        uint64_t grain = v.leaves.grain;
        uint64_t end   = v.plan.next + grain + (1u + 2u * v.plan.k) * grain + GROUND_WORKING_ROOM_BLOCKS + 1u;
        printf("%llu\n", (unsigned long long)((end + grain - 1u) / grain * grain * TAGFS_BLOCK_SECTORS));
    }
    V2Release(&v);
    return said ? 0 : 1;
}
