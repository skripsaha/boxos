/*
 * create_tagfs_v1_pack.c — making a format-v1 volume, in the order its parts
 * go down.
 *
 * The registry and the data first, the pool and the table that name the
 * files, the integrity map, the bitmap that marks all of it used, the
 * DiskBook, then the Ledger that says what is true of the volume, and the
 * Deed at both ends last of all. What one part leaves for the next — the
 * high-water mark, the seal, the uuid — travels in the pack.
 */

#include "create_tagfs_v1.h"

typedef struct {
    FILE*           disk;
    VolumeShape     shape;
    uint32_t        total_blocks;       /* the data run, in blocks          */
    uint32_t        data_start_block;   /* where it begins, in the volume's */
    int             file_count;
    FileInfo*       files;
    int             kernel_file_index;
    uint32_t        next_block;         /* the high-water mark of the run   */
    SettleMark      seal;
    uint32_t        reg_blocks;
    uint32_t        mpool_block_count;
    FileTableEntry* ftable_all;         /* indexed by file id               */
    uint32_t        ftable_entries;
    uint32_t        ftable_blocks;
    uint32_t        map_first;
    uint32_t        map_count;
    uint8_t         volume_uuid[16];
} V1Pack;

static int V1PackLay(V1Pack* p) {
    /* ---- Compute layout, in the volume's own blocks ---- */
    const VolumeShape* shape = &p->shape;
    int laid = compute_layout(g_ground_sectors, &p->shape);
    if (laid == -2) {
        fprintf(stderr,
                "The ground claimed for BoxOS is %llu sectors — more than the "
                "16 TiB a format-v1 volume counts its blocks in. It is refused "
                "rather than laid out over a part of it.\n",
                (unsigned long long)g_ground_sectors);
        return -1;
    }
    if (laid != 0) {
        fprintf(stderr,
                "The ground claimed for BoxOS is %llu sectors, and a volume's "
                "front matter alone needs more than that\n",
                (unsigned long long)g_ground_sectors);
        return -1;
    }
    p->total_blocks     = shape->data_blocks;
    p->data_start_block = shape->data_block;

    printf("  Deed:          block %u, far copy at sector %llu\n",
           shape->deed_block, (unsigned long long)shape->tail_sector);
    printf("  Ledger:        blocks %u..%u\n",
           shape->ledger_block, shape->ledger_block + VOLUME_LEDGER_COPIES - 1);
    printf("  DiskBook:      blocks %u..%u (head, backup, %u record blocks)\n",
           shape->disk_book_block,
           shape->disk_book_block + shape->disk_book_blocks - 1,
           DISK_BOOK_ENTRY_BLOCKS);
    printf("  Block bitmap:  blocks %u..%u\n",
           shape->bitmap_block, shape->bitmap_block + shape->bitmap_blocks - 1);
    printf("  Data:          blocks %u..%u — %u of them (%u MiB)\n",
           shape->data_block, shape->data_block + shape->data_blocks - 1,
           shape->data_blocks, (shape->data_blocks * 4) / 1024);
    printf("  Reserved:      data block 0 (registry), 1 (file table), 2 (metadata pool)\n");
    return 0;
}

static int V1PackPlace(V1Pack* p, char* argv[], int first) {
    /* ---- Parse files and tags ---- */
    if (p->file_count > 0) {
        p->files = calloc(p->file_count, sizeof(FileInfo));
        if (!p->files) { fprintf(stderr, "calloc failed\n"); return -1; }
    }

    /* Claim ids 0..11 for the reserved vocabulary BEFORE any file tag is
     * interned, so the kernel's privilege masks are representable on mount. */
    seed_reserved_tags();

    p->next_block = 3;  /* blocks 0,1,2 reserved */

    for (int i = 0; i < p->file_count; i++) {
        const char* filepath = argv[first + i * 2];
        const char* tags_str = argv[first + i * 2 + 1];
        FileInfo*   fi       = &p->files[i];

        FILE* f = fopen(filepath, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open: %s\n", filepath);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        fi->file_size = (uint64_t)ftell(f);
        fclose(f);

        fi->filepath    = strdup(filepath);
        const char* base = strrchr(filepath, '/');
        fi->filename    = strdup(base ? base + 1 : filepath);
        fi->file_id     = (uint32_t)(i + 1);  /* file IDs start from 1 */
        fi->block_count = (uint32_t)((fi->file_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
        if (fi->block_count == 0) fi->block_count = 1;
        fi->start_block = p->next_block;

        parse_tags(tags_str, fi->filename, &fi->tag_ids, &fi->tag_count);

        if (KernelTagged(fi->tag_ids, fi->tag_count)) p->kernel_file_index = i;

        if (p->next_block + fi->block_count > p->total_blocks) {
            fprintf(stderr, "Not enough blocks: need %u more, only %u total\n",
                    p->next_block + fi->block_count, p->total_blocks);
            return -1;
        }

        p->next_block += fi->block_count;

        printf("  File %2d: %-30s  id=%u  size=%-8lu  blocks=%u-%u  tags=%s\n",
               i + 1, fi->filename, fi->file_id, (unsigned long)fi->file_size,
               fi->start_block, fi->start_block + fi->block_count - 1,
               tags_str);
    }

    printf("  Tags interned: %u\n", g_tag_count);
    printf("  Blocks used:   %u / %u (3 reserved + %u data)\n",
           p->next_block, p->total_blocks, p->next_block - 3);
    return 0;
}

static int V1PackRegistry(V1Pack* p) {
    /* ---- Write tag registry (block 0, chains if needed) ---- */
    printf("\n[create_tagfs] Writing tag registry...\n");
    p->reg_blocks = 1;
    memset(&p->seal, 0, sizeof(p->seal));
    if (write_registry_chain(p->disk, p->data_start_block, &p->next_block,
                             p->total_blocks, &p->reg_blocks, &p->seal) != 0) {
        return -1;
    }
    if (p->reg_blocks > 1) {
        printf("  Tag registry: %u tags over %u blocks\n", g_tag_count, p->reg_blocks);
    }
    return 0;
}

static int V1PackData(V1Pack* p) {
    /* ---- Write file data (blocks 3+) ---- */
    printf("[create_tagfs] Writing file data...\n");
    for (int i = 0; i < p->file_count; i++) {
        FILE* f = fopen(p->files[i].filepath, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for data write\n", p->files[i].filepath);
            return -1;
        }
        if (write_file_data(p->disk, p->data_start_block, p->files[i].start_block,
                            f, p->files[i].file_size, p->files[i].block_count) != 0) {
            fprintf(stderr, "Failed to write data for %s\n", p->files[i].filename);
            fclose(f); return -1;
        }
        fclose(f);
    }
    return 0;
}

/* The pool's block is full: link it to a new one at the high-water mark, write
 * it, fold it into the seal, and start the new one. */
static int V1PackPoolChain(V1Pack* p, MetaPoolBlock* mpool, uint32_t* current_mpool_block) {
    uint32_t new_mpool_block = p->next_block++;
    if (new_mpool_block >= p->total_blocks) {
        fprintf(stderr, "Not enough blocks for metadata pool chain\n");
        return -1;
    }

    /* Link old block → new block, write old block to disk */
    mpool->next_block = new_mpool_block;
    if (write_block(p->disk, p->data_start_block, *current_mpool_block, mpool) != 0) {
        fprintf(stderr, "Failed to write metadata pool block %u\n", *current_mpool_block);
        return -1;
    }
    printf("  Metadata pool: block %u full (%u records, %u bytes), chaining to block %u\n",
           *current_mpool_block, mpool->record_count, mpool->used_bytes, new_mpool_block);
    p->seal.closed_crc32 ^= pool_block_mix(*current_mpool_block, mpool);

    /* Start fresh block */
    *current_mpool_block = new_mpool_block;
    memset(mpool, 0, sizeof(MetaPoolBlock));
    mpool->magic = TAGFS_MPOOL_MAGIC;
    p->mpool_block_count++;
    return 0;
}

static int V1PackPool(V1Pack* p) {
    /* ---- Write metadata pool (block 2, chains if needed) ---- */
    printf("[create_tagfs] Writing metadata pool...\n");
    MetaPoolBlock mpool;
    memset(&mpool, 0, sizeof(MetaPoolBlock));
    mpool.magic = TAGFS_MPOOL_MAGIC;

    uint32_t current_mpool_block = 2;  /* first mpool block */
    p->mpool_block_count         = 1;  /* how many mpool blocks total */

    /*
     * ‼ THE FILE TABLE IS AS LONG AS THE FILE IDS NEED, AND IT CHAINS.
     *
     * This used to be one FileTableBlock — 510 entries — and a file whose id
     * fell outside it was simply not written into the table, and its entry
     * count was clamped on the way out. An image built with more than 510 files
     * shipped with the surplus already unreachable and said nothing about it.
     *
     * So the entries are built in one flat array indexed by file id, and laid
     * onto as many blocks as that takes, linked the same way the metadata pool
     * links its own. The continuation blocks come out of the same high-water
     * mark the pool's do, so the bitmap written below marks them used.
     */
    p->ftable_entries = p->file_count > 0
                      ? (uint32_t)p->files[p->file_count - 1].file_id + 1u
                      : 0u;
    p->ftable_all = calloc(p->ftable_entries ? p->ftable_entries : 1,
                           sizeof(FileTableEntry));
    if (!p->ftable_all) {
        fprintf(stderr, "calloc failed for the file table\n");
        return -1;
    }

    for (int i = 0; i < p->file_count; i++) {
        uint8_t record_buf[4096];
        uint32_t record_size = pack_metadata_record(record_buf, &p->files[i]);

        if (record_size > TAGFS_MPOOL_DATA_SIZE) {
            fprintf(stderr, "Metadata record too large for %s (%u bytes)\n",
                    p->files[i].filename, record_size);
            return -1;
        }

        /* If current block is full, chain to a new block */
        if (mpool.used_bytes + record_size > TAGFS_MPOOL_DATA_SIZE &&
            V1PackPoolChain(p, &mpool, &current_mpool_block) != 0) {
            return -1;
        }

        /* meta_offset = MPOOL_BLOCK_HEADER + current used_bytes */
        uint32_t meta_offset = MPOOL_BLOCK_HEADER + mpool.used_bytes;
        memcpy(mpool.payload + mpool.used_bytes, record_buf, record_size);
        mpool.used_bytes += (uint16_t)record_size;
        mpool.record_count++;
        mpool.kept++;

        /* File table: entry at index file_id */
        uint32_t fid = p->files[i].file_id;
        if (fid < p->ftable_entries) {
            p->ftable_all[fid].meta_block  = current_mpool_block;
            p->ftable_all[fid].meta_offset = meta_offset;
        }
    }

    /* Write final (or only) mpool block */
    if (write_block(p->disk, p->data_start_block, current_mpool_block, &mpool) != 0) {
        fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
        return -1;
    }
    p->seal.chain_first     = 2;
    p->seal.pool_from_block = 2;
    p->seal.pool_from_bytes = 0;
    p->seal.pool_to_block   = current_mpool_block;
    p->seal.pool_to_bytes   = mpool.used_bytes;
    p->seal.tail_crc32      = tagfs_crc32(mpool.payload, mpool.used_bytes);
    if (p->mpool_block_count > 1) {
        printf("  Metadata pool: %u blocks total\n", p->mpool_block_count);
    }
    return 0;
}

static int V1PackTable(V1Pack* p) {
    /* ---- Write file table (block 1, chains if needed) ---- */
    printf("[create_tagfs] Writing file table...\n");
    p->ftable_blocks =
        (p->ftable_entries + TAGFS_FTABLE_PER_BLOCK - 1) / TAGFS_FTABLE_PER_BLOCK;
    if (p->ftable_blocks == 0) p->ftable_blocks = 1;

    uint32_t here = 1;   /* the first file table block, where the Deed says */
    for (uint32_t bi = 0; bi < p->ftable_blocks; bi++) {
        uint32_t taken = bi * TAGFS_FTABLE_PER_BLOCK;
        uint32_t count = p->ftable_entries > taken ? p->ftable_entries - taken : 0;
        if (count > TAGFS_FTABLE_PER_BLOCK) count = TAGFS_FTABLE_PER_BLOCK;

        uint32_t next = 0;
        if (bi + 1 < p->ftable_blocks) {
            next = p->next_block++;
            if (next >= p->total_blocks) {
                fprintf(stderr, "Not enough blocks for the file table chain\n");
                return -1;
            }
        }

        FileTableBlock ftable;
        memset(&ftable, 0, sizeof(FileTableBlock));
        ftable.magic       = TAGFS_FILETBL_MAGIC;
        ftable.next_block  = next;
        ftable.entry_count = count;
        for (uint32_t i = 0; i < count; i++) {
            ftable.entries[i] = p->ftable_all[taken + i];
        }

        if (write_block(p->disk, p->data_start_block, here, &ftable) != 0) {
            fprintf(stderr, "Failed to write file table block %u\n", here);
            return -1;
        }
        here = next;
    }
    if (p->ftable_blocks > 1) {
        printf("  File table: %u entries over %u blocks\n",
               p->ftable_entries, p->ftable_blocks);
    }
    free(p->ftable_all);
    p->ftable_all = NULL;
    return 0;
}

static int V1PackMap(V1Pack* p) {
    /* ---- The integrity map, laid zeroed where the Ledger names it ---- */
    printf("[create_tagfs] Writing the integrity map...\n");
    p->map_first = p->next_block;
    p->map_count = (p->total_blocks + INTEGRITY_DIGESTS_PER_BLOCK - 1) / INTEGRITY_DIGESTS_PER_BLOCK;
    if (write_integrity_map(p->disk, p->data_start_block, p->map_first, p->map_count,
                            p->total_blocks) != 0) {
        return -1;
    }
    p->next_block += p->map_count;
    return 0;
}

static int V1PackBitmap(V1Pack* p) {
    /* ---- Write block bitmap ---- */
    printf("[create_tagfs] Writing block bitmap...\n");
    uint32_t bitmap_buf_size = p->shape.bitmap_blocks * TAGFS_BLOCK_SIZE;
    uint8_t* bitmap = calloc(1, bitmap_buf_size);
    if (!bitmap) { fprintf(stderr, "calloc failed for bitmap\n"); return -1; }

    /* Mark all used blocks: 0=registry, 1=ftable, 2=mpool, plus any chained
       mpool and file table blocks and all file data blocks. next_block tracks
       the high-water mark. */
    for (uint32_t b = 0; b < p->next_block && b < p->total_blocks; b++) {
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    }

    if (write_at_sector(p->disk, (uint64_t)p->shape.bitmap_block * TAGFS_BLOCK_SECTORS,
                        bitmap, bitmap_buf_size) != 0) {
        fprintf(stderr, "Failed to write block bitmap\n");
        free(bitmap); return -1;
    }
    free(bitmap);
    return 0;
}

static int V1PackDiskBook(V1Pack* p) {
    /* ---- Write the DiskBook's head, its backup, and an empty record run ---- */
    printf("[create_tagfs] Writing DiskBook...\n");
    /* Must match the kernel's DiskBookSuperblock (disk_book.h):
     *   u32 magic, u32 version, u64 start_sector, u32 capacity,
     *   u32 count, u32 generation, u32 flags, u32 crc32, u8 uuid[16], ...
     * `start_sector` is counted from the start of the volume, like every
     * other number the volume keeps about itself. */
    uint8_t dbuf[TAGFS_SECTOR_BYTES];
    memset(dbuf, 0, sizeof(dbuf));
    uint32_t dmag   = DISK_BOOK_SB_MAGIC;
    uint32_t dver   = DISK_BOOK_VERSION;
    uint64_t dstart = (uint64_t)(p->shape.disk_book_block + 2) * TAGFS_BLOCK_SECTORS;
    uint32_t dcap   = DISK_BOOK_CAPACITY;
    uint32_t dcount = 0;
    memcpy(dbuf + 0,  &dmag,   4);
    memcpy(dbuf + 4,  &dver,   4);
    memcpy(dbuf + 8,  &dstart, 8);
    memcpy(dbuf + 16, &dcap,   4);
    memcpy(dbuf + 20, &dcount, 4);
    /* crc32 at offset 32 (after generation@24, flags@28), over the 512-byte
     * block with the crc field zeroed — matches the kernel's DiskBook SB CRC
     * so a freshly-imaged volume validates without a first-mount reformat. */
    uint32_t dcrc = tagfs_crc32(dbuf, sizeof(dbuf));
    memcpy(dbuf + 32, &dcrc, 4);

    /* A block apart, not a sector: the head and the copy of the head were
     * inside one physical block, so the copy died with what it was for. */
    if (write_at_sector(p->disk, (uint64_t)p->shape.disk_book_block * TAGFS_BLOCK_SECTORS,
                        dbuf, sizeof(dbuf)) != 0 ||
        write_at_sector(p->disk, (uint64_t)(p->shape.disk_book_block + 1) * TAGFS_BLOCK_SECTORS,
                        dbuf, sizeof(dbuf)) != 0) {
        fprintf(stderr, "Failed to write the DiskBook's head and its copy\n");
        return -1;
    }

    uint8_t zero[TAGFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t b = 0; b < DISK_BOOK_ENTRY_BLOCKS; b++) {
        if (write_at_sector(p->disk,
                            (uint64_t)(p->shape.disk_book_block + 2 + b) * TAGFS_BLOCK_SECTORS,
                            zero, sizeof(zero)) != 0) {
            fprintf(stderr, "Failed to write the DiskBook's record block %u\n", b);
            return -1;
        }
    }
    return 0;
}

static int V1PackLedger(V1Pack* p) {
    /* ---- The Ledger: what is true of this volume the moment it is made ---- */
    printf("[create_tagfs] Writing the Ledger...\n");

    uint32_t used_blocks = p->next_block;
    uint32_t free_blocks = p->total_blocks - used_blocks;

    uint64_t made_at = (uint64_t)time(NULL);

    if (generate_uuid(p->volume_uuid) != 0) return -1;

    VolumeLedger led;
    memset(&led, 0, sizeof(led));
    led.seq          = 1;
    led.written_unix = made_at;
    led.free_blocks  = free_blocks;
    led.total_files  = (uint64_t)p->file_count;
    led.next_file_id = (uint32_t)(p->file_count + 1);
    led.next_tag_id  = g_tag_count;
    led.total_tags   = g_tag_count;
    led.integrity_map_block  = p->map_first;
    led.integrity_map_blocks = p->map_count;
    led.mark         = p->seal;
    led.mark.settle  = led.seq;
    led.mark.flags   = SETTLE_CLEAN;

    if (write_ledger(p->disk, &p->shape, &led) != 0) {
        fprintf(stderr, "Failed to write the Ledger\n");
        return -1;
    }
    return 0;
}

static int V1PackDeed(V1Pack* p, char* argv[], int first) {
    /* ---- The Deed: at the head of the ground, and again at the far end ---- */
    printf("[create_tagfs] Writing the Deed...\n");

    VolumeBoot boot;
    memset(&boot, 0, sizeof(boot));
    if (p->kernel_file_index >= 0) {
        FileInfo* kf = &p->files[p->kernel_file_index];
        /* In the volume's own blocks, not the data run's — so a loader that has
         * the Deed and nothing else can turn it into a sector without first
         * understanding where the data run begins. */
        boot.kernel_block  = p->shape.data_block + kf->start_block;
        boot.kernel_blocks = kf->block_count;
        boot.kernel_bytes  = (uint32_t)kf->file_size;
        printf("  Boot:          kernel at volume block %u (%u blocks, %u bytes)\n",
               boot.kernel_block, boot.kernel_blocks, boot.kernel_bytes);
    } else {
        printf("  WARNING: nothing here is tagged 'kernel' — this volume carries "
               "no kernel and no loader will boot from it\n");
    }

    VolumePacked packed = PackedStamp(p->file_count, argv, first,
                                      p->kernel_file_index >= 0
                                      ? p->files[p->kernel_file_index].filepath : NULL);

    if (write_deed(p->disk, VOLUME_DEED_ROLE_HEAD, 0,
                   p->volume_uuid, &p->shape, &boot, p->mpool_block_count,
                   p->ftable_blocks, p->reg_blocks, &packed) != 0) {
        fprintf(stderr, "Failed to write the Deed\n");
        return -1;
    }
    if (write_deed(p->disk, VOLUME_DEED_ROLE_TAIL, p->shape.tail_sector,
                   p->volume_uuid, &p->shape, &boot, p->mpool_block_count,
                   p->ftable_blocks, p->reg_blocks, &packed) != 0) {
        fprintf(stderr, "Failed to write the far copy of the Deed\n");
        return -1;
    }
    return 0;
}

int V1PackMake(const char* disk_path, int file_count, char* argv[], int first) {
    V1Pack p;
    memset(&p, 0, sizeof(p));
    p.file_count        = file_count;
    p.kernel_file_index = -1;
    p.disk              = GroundOpen(disk_path);
    if (!p.disk) return 1;

    int made = V1PackLay(&p) == 0 && V1PackPlace(&p, argv, first) == 0 &&
               V1PackRegistry(&p) == 0 && V1PackData(&p) == 0 &&
               V1PackPool(&p) == 0 && V1PackTable(&p) == 0 &&
               V1PackMap(&p) == 0 && V1PackBitmap(&p) == 0 &&
               V1PackDiskBook(&p) == 0 && V1PackLedger(&p) == 0 &&
               V1PackDeed(&p, argv, first) == 0;
    /* The writes are buffered, so the close is where a medium that did not
     * take them can be the first to say so. */
    if (fclose(p.disk) != 0 && made) {
        fprintf(stderr, "Failed to finish writing the volume on %s\n", disk_path);
        made = 0;
    }

    if (made) {
        uint32_t used_blocks = p.next_block;
        printf("\n[create_tagfs] The volume is made.\n");
        printf("  Files:  %d\n", file_count);
        printf("  Tags:   %u\n", g_tag_count);
        printf("  Blocks: %u used / %u data (%u free)\n", used_blocks, p.total_blocks,
               p.total_blocks - used_blocks);
    }

    /* Cleanup */
    for (int i = 0; p.files && i < file_count; i++) {
        free(p.files[i].filepath);
        free(p.files[i].filename);
        free(p.files[i].tag_ids);
    }
    free(p.files);
    free(p.ftable_all);
    CargoRelease();

    return made ? 0 : 1;
}
