/*
 * create_tagfs.c — Format a TagFS filesystem on a disk image.
 *
 * Writes the NEW on-disk format (version 1) matching the kernel's tagfs.h:
 *   - TagFSSuperblock at sector 1034 (primary) and 1035 (backup)
 *   - TagRegistryBlock at data block 0
 *   - FileTableBlock at data block 1
 *   - MetaPoolBlock at data block 2
 *   - Block bitmap starting at sector 2062
 *   - File data at blocks 3+
 *   - Boot hints in superblock reserved[0..15] for stage2 bootloader
 *
 * Usage: create_tagfs <disk_image> [<file> <tags>] ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Constants — must match kernel's tagfs.h and boxos_magic.h
 * ==================================================================== */

#define TAGFS_MAGIC            0x54414746  /* "TAGF" */
#define TAGFS_REGISTRY_MAGIC   0x54524547  /* "TREG" */
#define TAGFS_FILETBL_MAGIC    0x54465442  /* "TFTB" */
#define TAGFS_MPOOL_MAGIC      0x544D504C  /* "TMPL" */
#define JOURNAL_MAGIC          0x4A4F5552  /* "JOUR" */

#define TAGFS_VERSION          1
#define TAGFS_BLOCK_SIZE       4096
#define TAGFS_FILE_ACTIVE      (1 << 0)

#define TAGFS_SUPERBLOCK_SECTOR       1034
#define TAGFS_BACKUP_SB_SECTOR        1035
#define TAGFS_JOURNAL_SB_SECTOR       1036
#define TAGFS_JOURNAL_BACKUP_SECTOR   1037
#define TAGFS_JOURNAL_ENTRIES_START   1038
#define TAGFS_JOURNAL_ENTRY_COUNT     512
#define TAGFS_BITMAP_SECTOR_START     2062  /* 1038 + 512*2 = 2062 */

#define TAGFS_REGISTRY_DATA_SIZE   4080
#define TAGFS_MPOOL_DATA_SIZE      4080
#define TAGFS_FTABLE_PER_BLOCK     510
#define TAGFS_INVALID_TAG_ID       0xFFFF

#define RECORD_HEADER_SIZE    42   /* metadata pool record header (40 fields + 2 CRC16) */
#define RECORD_CRC_OFFSET     40   /* CRC16 stored at bytes [40..41] of packed record */
#define MPOOL_BLOCK_HEADER    16   /* MetaPoolBlock header before payload */

/* Boot hint offsets within superblock (in reserved[] area, byte offset 108+) */
#define BOOT_HINT_KERNEL_BLOCK    0   /* reserved[0..3]  */
#define BOOT_HINT_KERNEL_BLOCKS   4   /* reserved[4..7]  */
#define BOOT_HINT_KERNEL_SIZE     8   /* reserved[8..11] */
#define BOOT_HINT_DATA_START     12   /* reserved[12..15] */

#define TAGFS_SB_CRC_OFFSET     400   /* CRC32 stored at reserved[400..403] */

/* ====================================================================
 * On-disk structures — must match kernel's tagfs.h exactly
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_files;
    uint32_t next_file_id;
    uint32_t next_tag_id;
    uint32_t total_tags;

    uint32_t tag_registry_block;
    uint32_t tag_registry_block_count;
    uint32_t file_table_block;
    uint32_t file_table_block_count;
    uint32_t metadata_pool_block;
    uint32_t metadata_pool_block_count;
    uint32_t block_bitmap_sector;
    uint32_t block_bitmap_sector_count;
    uint32_t journal_superblock_sector;

    uint64_t fs_created_time;
    uint64_t fs_modified_time;
    uint8_t  fs_uuid[16];
    uint32_t backup_superblock_sector;

    uint8_t  reserved[404];
} TagFSSuperblock;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t entry_count;
    uint16_t used_bytes;
    uint8_t  reserved[4];
    uint8_t  data[TAGFS_REGISTRY_DATA_SIZE];
} TagRegistryBlock;

typedef struct __attribute__((packed)) {
    uint32_t meta_block;
    uint32_t meta_offset;
} FileTableEntry;

typedef struct __attribute__((packed)) {
    uint32_t       magic;
    uint32_t       next_block;
    uint32_t       entry_count;
    uint32_t       reserved;
    FileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} FileTableBlock;

typedef struct __attribute__((packed)) {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint8_t  reserved[4];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} MetaPoolBlock;

/* ====================================================================
 * In-memory tracking
 * ==================================================================== */

typedef struct {
    char*    key;
    char*    value;   /* NULL for label tags */
    uint16_t tag_id;
} TagEntry;

static TagEntry* g_tags       = NULL;
static uint32_t  g_tag_count  = 0;
static uint32_t  g_tag_cap    = 0;

typedef struct {
    char*       filepath;
    char*       filename;
    uint32_t    file_id;
    uint64_t    file_size;
    uint32_t    start_block;
    uint32_t    block_count;
    uint16_t*   tag_ids;
    uint16_t    tag_count;
} FileInfo;

/* ====================================================================
 * Tag interning
 * ==================================================================== */

static uint16_t intern_tag(const char* key, const char* value) {
    /* Check for existing */
    for (uint32_t i = 0; i < g_tag_count; i++) {
        if (strcmp(g_tags[i].key, key) != 0) continue;
        if (value == NULL && g_tags[i].value == NULL) return g_tags[i].tag_id;
        if (value && g_tags[i].value && strcmp(g_tags[i].value, value) == 0)
            return g_tags[i].tag_id;
    }

    /* Add new */
    if (g_tag_count >= g_tag_cap) {
        g_tag_cap = g_tag_cap == 0 ? 32 : g_tag_cap * 2;
        g_tags = realloc(g_tags, sizeof(TagEntry) * g_tag_cap);
        if (!g_tags) { fprintf(stderr, "realloc failed\n"); exit(1); }
    }

    uint16_t id = (uint16_t)g_tag_count;
    g_tags[g_tag_count].key    = strdup(key);
    g_tags[g_tag_count].value  = value ? strdup(value) : NULL;
    g_tags[g_tag_count].tag_id = id;
    g_tag_count++;
    return id;
}

/* Extract filename stem: "kernel.bin" → "kernel", "files.elf" → "files" */
static void extract_stem(const char* filename, char* stem, size_t stem_size) {
    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= stem_size) len = stem_size - 1;
    memcpy(stem, base, len);
    stem[len] = '\0';
}

/* Check if tag_id already in array */
static int has_tag_id(const uint16_t* ids, uint16_t count, uint16_t id) {
    for (uint16_t i = 0; i < count; i++) {
        if (ids[i] == id) return 1;
    }
    return 0;
}

/* Parse comma-separated tag string like "system,type:elf,utility" */
static void parse_tags(const char* tag_string, const char* filename,
                        uint16_t** out_ids, uint16_t* out_count) {
    uint16_t  ids[256];
    uint16_t  count = 0;

    /* Auto-label tag from filename stem (always first) */
    char stem[256];
    extract_stem(filename, stem, sizeof(stem));
    if (stem[0] != '\0') {
        ids[count++] = intern_tag(stem, NULL);
    }

    char buf[4096];
    strncpy(buf, tag_string, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* token = strtok(buf, ",");
    while (token && count < 256) {
        while (*token == ' ') token++;

        uint16_t id;
        char* colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            id = intern_tag(token, colon + 1);
        } else {
            id = intern_tag(token, NULL);
        }

        /* Deduplicate: skip if already in the array */
        if (!has_tag_id(ids, count, id)) {
            ids[count++] = id;
        }
        token = strtok(NULL, ",");
    }

    *out_count = count;
    *out_ids = malloc(sizeof(uint16_t) * count);
    memcpy(*out_ids, ids, sizeof(uint16_t) * count);
}

/* ====================================================================
 * Disk helpers
 * ==================================================================== */

static int write_at_sector(FILE* disk, uint32_t sector, const void* data, size_t size) {
    if (fseek(disk, (long)sector * 512, SEEK_SET) != 0) return -1;
    if (fwrite(data, size, 1, disk) != 1) return -1;
    return 0;
}

static int write_block(FILE* disk, uint32_t data_start_sector, uint32_t block,
                        const void* data) {
    uint32_t sector = data_start_sector + block * 8;
    return write_at_sector(disk, sector, data, TAGFS_BLOCK_SIZE);
}

static int write_file_data(FILE* disk, uint32_t data_start_sector, uint32_t block,
                            FILE* src, uint64_t file_size, uint32_t block_count) {
    uint32_t sector = data_start_sector + block * 8;
    if (fseek(disk, (long)sector * 512, SEEK_SET) != 0) return -1;

    uint8_t buf[TAGFS_BLOCK_SIZE];
    uint64_t remaining = file_size;

    for (uint32_t i = 0; i < block_count; i++) {
        memset(buf, 0, TAGFS_BLOCK_SIZE);
        size_t to_read = remaining > TAGFS_BLOCK_SIZE ? TAGFS_BLOCK_SIZE : (size_t)remaining;
        if (to_read > 0) {
            size_t got = fread(buf, 1, to_read, src);
            if (got != to_read) {
                fprintf(stderr, "Short read: got %zu, expected %zu\n", got, to_read);
                return -1;
            }
            remaining -= got;
        }
        if (fwrite(buf, TAGFS_BLOCK_SIZE, 1, disk) != 1) return -1;
    }
    return 0;
}

/* ====================================================================
 * Layout computation
 * ==================================================================== */

static void compute_layout(uint32_t disk_sectors,
                            uint32_t* out_total_blocks,
                            uint32_t* out_bitmap_sectors,
                            uint32_t* out_data_start_sector) {
    /* Iterate to convergence (typically 1-2 iterations) */
    uint32_t bitmap_sectors = 1;
    uint32_t data_start, total_blocks;

    for (int i = 0; i < 10; i++) {
        data_start   = TAGFS_BITMAP_SECTOR_START + bitmap_sectors;
        total_blocks = (disk_sectors - data_start) / 8;

        uint32_t bitmap_bytes   = (total_blocks + 7) / 8;
        uint32_t new_bm_sectors = (bitmap_bytes + 511) / 512;

        if (new_bm_sectors == bitmap_sectors) break;
        bitmap_sectors = new_bm_sectors;
    }

    *out_total_blocks      = total_blocks;
    *out_bitmap_sectors    = bitmap_sectors;
    *out_data_start_sector = TAGFS_BITMAP_SECTOR_START + bitmap_sectors;
}

/* ====================================================================
 * Build tag registry block
 * ==================================================================== */

static int build_registry_block(TagRegistryBlock* blk) {
    memset(blk, 0, sizeof(TagRegistryBlock));
    blk->magic = TAGFS_REGISTRY_MAGIC;

    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_tag_count; i++) {
        uint8_t key_len   = (uint8_t)strlen(g_tags[i].key);
        uint16_t value_len = g_tags[i].value ? (uint16_t)strlen(g_tags[i].value) : 0;
        uint8_t flags      = g_tags[i].value ? 1 : 0;

        uint32_t record_size = 2 + 1 + 1 + 2 + key_len + value_len;
        if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
            fprintf(stderr, "Tag registry block overflow (%u tags, %u bytes used)\n",
                    i, offset);
            return -1;
        }

        uint8_t* p = blk->data + offset;

        /* tag_id (uint16_t) — ignored by kernel but write it anyway */
        memcpy(p, &g_tags[i].tag_id, 2); p += 2;
        /* flags (uint8_t) */
        *p++ = flags;
        /* key_len (uint8_t) */
        *p++ = key_len;
        /* value_len (uint16_t) */
        memcpy(p, &value_len, 2); p += 2;
        /* key */
        memcpy(p, g_tags[i].key, key_len); p += key_len;
        /* value */
        if (value_len > 0) {
            memcpy(p, g_tags[i].value, value_len);
        }

        offset += record_size;
    }

    blk->entry_count = (uint16_t)g_tag_count;
    blk->used_bytes  = (uint16_t)offset;
    return 0;
}

/* CRC-16/CCITT-FALSE — must match kernel's meta_crc16 exactly */
static uint16_t meta_crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

/* ====================================================================
 * Build metadata pool record (matches kernel's pack_record exactly)
 * ==================================================================== */

static uint32_t pack_metadata_record(uint8_t* buf, const FileInfo* fi) {
    const char* fname = fi->filename;
    uint16_t name_len = (uint16_t)strlen(fname);
    uint16_t extent_count = 1;  /* single contiguous extent */

    uint16_t record_len = (uint16_t)(RECORD_HEADER_SIZE
                          + fi->tag_count * sizeof(uint16_t)
                          + extent_count * sizeof(FileExtent)
                          + name_len);

    uint32_t pos = 0;

    /* record_len */
    memcpy(buf + pos, &record_len, 2);            pos += 2;
    /* file_id */
    memcpy(buf + pos, &fi->file_id, 4);           pos += 4;
    /* flags */
    uint32_t flags = TAGFS_FILE_ACTIVE;
    memcpy(buf + pos, &flags, 4);                 pos += 4;
    /* size */
    memcpy(buf + pos, &fi->file_size, 8);         pos += 8;
    /* created_time */
    uint64_t now = (uint64_t)time(NULL);
    memcpy(buf + pos, &now, 8);                   pos += 8;
    /* modified_time */
    memcpy(buf + pos, &now, 8);                   pos += 8;
    /* tag_count */
    memcpy(buf + pos, &fi->tag_count, 2);         pos += 2;
    /* extent_count */
    memcpy(buf + pos, &extent_count, 2);          pos += 2;
    /* name_len */
    memcpy(buf + pos, &name_len, 2);              pos += 2;
    /* CRC16 placeholder — zeroed for computation, stamped below */
    uint16_t zero_crc = 0;
    memcpy(buf + pos, &zero_crc, 2);              pos += 2;

    /* tag_ids[] */
    if (fi->tag_count > 0) {
        memcpy(buf + pos, fi->tag_ids, fi->tag_count * 2);
        pos += fi->tag_count * 2;
    }

    /* extents[] — single extent: {start_block, block_count} */
    FileExtent ext;
    ext.start_block = fi->start_block;
    ext.block_count = (uint16_t)fi->block_count;
    memcpy(buf + pos, &ext, sizeof(FileExtent));
    pos += sizeof(FileExtent);

    /* filename */
    memcpy(buf + pos, fname, name_len);
    pos += name_len;

    /* Stamp CRC16 over the entire record (with CRC field zeroed) */
    uint16_t crc = meta_crc16(buf, (uint32_t)record_len);
    memcpy(buf + RECORD_CRC_OFFSET, &crc, 2);

    return (uint32_t)record_len;
}

/* ====================================================================
 * UUID generation
 * ==================================================================== */

static void generate_uuid(uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) uuid[i] = rand() & 0xFF;
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

/* ====================================================================
 * CRC32 (ISO 3309 — must match kernel's tagfs_crc32 exactly)
 * ==================================================================== */

static uint32_t tagfs_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

static void superblock_stamp_crc(TagFSSuperblock* sb) {
    memset(sb->reserved + TAGFS_SB_CRC_OFFSET, 0, 4);
    uint32_t crc = tagfs_crc32((const uint8_t*)sb, sizeof(TagFSSuperblock));
    memcpy(sb->reserved + TAGFS_SB_CRC_OFFSET, &crc, 4);
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char* argv[]) {
    if (argc < 2 || (argc > 2 && (argc - 2) % 2 != 0)) {
        fprintf(stderr, "Usage: %s <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Creates TagFS v1 filesystem on a disk image.\n");
        fprintf(stderr, "Layout is fixed: superblock=1034, bitmap=2062, blocks after bitmap.\n");
        fprintf(stderr, "First file tagged 'kernel' gets boot hints in superblock.\n");
        return 1;
    }

    const char* disk_path = argv[1];
    int file_count = (argc - 2) / 2;

    /* ---- Open disk ---- */
    FILE* disk = fopen(disk_path, "r+b");
    if (!disk) { perror("Failed to open disk image"); return 1; }

    fseek(disk, 0, SEEK_END);
    long disk_size = ftell(disk);
    uint32_t disk_sectors = (uint32_t)(disk_size / 512);

    printf("[create_tagfs] Formatting TagFS v1 on %s (%u sectors, %lu bytes)\n",
           disk_path, disk_sectors, disk_size);

    /* ---- Compute layout ---- */
    uint32_t total_blocks, bitmap_sectors, data_start_sector;
    compute_layout(disk_sectors, &total_blocks, &bitmap_sectors, &data_start_sector);

    printf("  Superblock:    sector %u (backup %u)\n", TAGFS_SUPERBLOCK_SECTOR, TAGFS_BACKUP_SB_SECTOR);
    printf("  Journal:       sector %u (backup %u, entries %u-%u)\n",
           TAGFS_JOURNAL_SB_SECTOR, TAGFS_JOURNAL_BACKUP_SECTOR,
           TAGFS_JOURNAL_ENTRIES_START,
           TAGFS_JOURNAL_ENTRIES_START + TAGFS_JOURNAL_ENTRY_COUNT * 2 - 1);
    printf("  Block bitmap:  sector %u (%u sectors)\n", TAGFS_BITMAP_SECTOR_START, bitmap_sectors);
    printf("  Data start:    sector %u\n", data_start_sector);
    printf("  Total blocks:  %u (%u MB)\n", total_blocks, (total_blocks * 4) / 1024);
    printf("  Reserved:      block 0 (registry), 1 (file table), 2 (metadata pool)\n");

    /* ---- Parse files and tags ---- */
    FileInfo* files = NULL;
    if (file_count > 0) {
        files = calloc(file_count, sizeof(FileInfo));
        if (!files) { fprintf(stderr, "calloc failed\n"); fclose(disk); return 1; }
    }

    uint32_t next_block = 3;  /* blocks 0,1,2 reserved */
    int kernel_file_index = -1;

    for (int i = 0; i < file_count; i++) {
        const char* filepath = argv[2 + i * 2];
        const char* tags_str = argv[2 + i * 2 + 1];

        FILE* f = fopen(filepath, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open: %s\n", filepath);
            fclose(disk); return 1;
        }
        fseek(f, 0, SEEK_END);
        files[i].file_size = (uint64_t)ftell(f);
        fclose(f);

        files[i].filepath    = strdup(filepath);
        const char* base     = strrchr(filepath, '/');
        files[i].filename    = strdup(base ? base + 1 : filepath);
        files[i].file_id     = (uint32_t)(i + 1);  /* file IDs start from 1 */
        files[i].block_count = (uint32_t)((files[i].file_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
        if (files[i].block_count == 0) files[i].block_count = 1;
        files[i].start_block = next_block;

        parse_tags(tags_str, files[i].filename, &files[i].tag_ids, &files[i].tag_count);

        /* Check if this file has a "kernel" tag → use for boot hints */
        for (uint16_t t = 0; t < files[i].tag_count; t++) {
            if (g_tags[files[i].tag_ids[t]].value == NULL &&
                strcmp(g_tags[files[i].tag_ids[t]].key, "kernel") == 0) {
                kernel_file_index = i;
                break;
            }
        }

        if (next_block + files[i].block_count > total_blocks) {
            fprintf(stderr, "Not enough blocks: need %u more, only %u total\n",
                    next_block + files[i].block_count, total_blocks);
            fclose(disk); return 1;
        }

        next_block += files[i].block_count;

        printf("  File %2d: %-30s  id=%u  size=%-8lu  blocks=%u-%u  tags=%s\n",
               i + 1, files[i].filename, files[i].file_id, (unsigned long)files[i].file_size,
               files[i].start_block, files[i].start_block + files[i].block_count - 1,
               tags_str);
    }

    printf("  Tags interned: %u\n", g_tag_count);
    printf("  Blocks used:   %u / %u (3 reserved + %u data)\n",
           next_block, total_blocks, next_block - 3);

    /* ---- Write tag registry block (block 0) ---- */
    printf("\n[create_tagfs] Writing tag registry...\n");
    TagRegistryBlock reg_blk;
    if (build_registry_block(&reg_blk) != 0) {
        fclose(disk); return 1;
    }
    if (write_block(disk, data_start_sector, 0, &reg_blk) != 0) {
        fprintf(stderr, "Failed to write registry block\n");
        fclose(disk); return 1;
    }

    /* ---- Write file data (blocks 3+) ---- */
    printf("[create_tagfs] Writing file data...\n");
    for (int i = 0; i < file_count; i++) {
        FILE* f = fopen(files[i].filepath, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for data write\n", files[i].filepath);
            fclose(disk); return 1;
        }
        if (write_file_data(disk, data_start_sector, files[i].start_block,
                            f, files[i].file_size, files[i].block_count) != 0) {
            fprintf(stderr, "Failed to write data for %s\n", files[i].filename);
            fclose(f); fclose(disk); return 1;
        }
        fclose(f);
    }

    /* ---- Write metadata pool (block 2, chains if needed) ---- */
    printf("[create_tagfs] Writing metadata pool...\n");
    MetaPoolBlock mpool;
    memset(&mpool, 0, sizeof(MetaPoolBlock));
    mpool.magic = TAGFS_MPOOL_MAGIC;

    uint32_t current_mpool_block = 2;  /* first mpool block */
    uint32_t mpool_block_count   = 1;  /* how many mpool blocks total */

    /* Also build file table entries as we go */
    FileTableBlock ftable;
    memset(&ftable, 0, sizeof(FileTableBlock));
    ftable.magic = TAGFS_FILETBL_MAGIC;

    for (int i = 0; i < file_count; i++) {
        uint8_t record_buf[4096];
        uint32_t record_size = pack_metadata_record(record_buf, &files[i]);

        if (record_size > TAGFS_MPOOL_DATA_SIZE) {
            fprintf(stderr, "Metadata record too large for %s (%u bytes)\n",
                    files[i].filename, record_size);
            fclose(disk); return 1;
        }

        /* If current block is full, chain to a new block */
        if (mpool.used_bytes + record_size > TAGFS_MPOOL_DATA_SIZE) {
            uint32_t new_mpool_block = next_block++;
            if (new_mpool_block >= total_blocks) {
                fprintf(stderr, "Not enough blocks for metadata pool chain\n");
                fclose(disk); return 1;
            }

            /* Link old block → new block, write old block to disk */
            mpool.next_block = new_mpool_block;
            if (write_block(disk, data_start_sector, current_mpool_block, &mpool) != 0) {
                fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
                fclose(disk); return 1;
            }
            printf("  Metadata pool: block %u full (%u records, %u bytes), chaining to block %u\n",
                   current_mpool_block, mpool.record_count, mpool.used_bytes, new_mpool_block);

            /* Start fresh block */
            current_mpool_block = new_mpool_block;
            memset(&mpool, 0, sizeof(MetaPoolBlock));
            mpool.magic = TAGFS_MPOOL_MAGIC;
            mpool_block_count++;
        }

        /* meta_offset = MPOOL_BLOCK_HEADER + current used_bytes */
        uint32_t meta_offset = MPOOL_BLOCK_HEADER + mpool.used_bytes;
        memcpy(mpool.payload + mpool.used_bytes, record_buf, record_size);
        mpool.used_bytes += (uint16_t)record_size;
        mpool.record_count++;

        /* File table: entry at index file_id */
        uint32_t fid = files[i].file_id;
        if (fid < TAGFS_FTABLE_PER_BLOCK) {
            ftable.entries[fid].meta_block  = current_mpool_block;
            ftable.entries[fid].meta_offset = meta_offset;
        }
    }

    ftable.entry_count = file_count > 0 ? files[file_count - 1].file_id + 1 : 0;
    if (ftable.entry_count > TAGFS_FTABLE_PER_BLOCK) {
        ftable.entry_count = TAGFS_FTABLE_PER_BLOCK;
    }

    /* Write final (or only) mpool block */
    if (write_block(disk, data_start_sector, current_mpool_block, &mpool) != 0) {
        fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
        fclose(disk); return 1;
    }
    if (mpool_block_count > 1) {
        printf("  Metadata pool: %u blocks total\n", mpool_block_count);
    }

    /* ---- Write file table (block 1) ---- */
    printf("[create_tagfs] Writing file table...\n");
    if (write_block(disk, data_start_sector, 1, &ftable) != 0) {
        fprintf(stderr, "Failed to write file table block\n");
        fclose(disk); return 1;
    }

    /* ---- Write block bitmap ---- */
    printf("[create_tagfs] Writing block bitmap...\n");
    uint32_t bitmap_buf_size = bitmap_sectors * 512;
    uint8_t* bitmap = calloc(1, bitmap_buf_size);
    if (!bitmap) { fprintf(stderr, "calloc failed for bitmap\n"); fclose(disk); return 1; }

    /* Mark all used blocks: 0=registry, 1=ftable, 2=mpool, plus any chained mpool blocks
       and all file data blocks. next_block tracks the high-water mark. */
    for (uint32_t b = 0; b < next_block && b < total_blocks; b++) {
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    }

    if (write_at_sector(disk, TAGFS_BITMAP_SECTOR_START, bitmap, bitmap_buf_size) != 0) {
        fprintf(stderr, "Failed to write block bitmap\n");
        free(bitmap); fclose(disk); return 1;
    }
    free(bitmap);

    /* ---- Write journal superblock (empty, with proper fields) ---- */
    printf("[create_tagfs] Writing journal superblock...\n");
    {
        uint8_t jbuf[512];
        memset(jbuf, 0, 512);
        uint32_t jmag  = JOURNAL_MAGIC;
        uint32_t jver  = 2;
        uint32_t jstart = TAGFS_JOURNAL_ENTRIES_START;
        uint32_t jcount = TAGFS_JOURNAL_ENTRY_COUNT;
        uint32_t jhead = 0;
        uint32_t jtail = 0;
        uint32_t jseq  = 1;
        memcpy(jbuf + 0,  &jmag,   4);
        memcpy(jbuf + 4,  &jver,   4);
        memcpy(jbuf + 8,  &jstart, 4);
        memcpy(jbuf + 12, &jcount, 4);
        memcpy(jbuf + 16, &jhead,  4);
        memcpy(jbuf + 20, &jtail,  4);
        memcpy(jbuf + 24, &jseq,   4);

        write_at_sector(disk, TAGFS_JOURNAL_SB_SECTOR, jbuf, 512);
        write_at_sector(disk, TAGFS_JOURNAL_BACKUP_SECTOR, jbuf, 512);
    }

    /* Zero journal entries area */
    {
        uint8_t zero[512];
        memset(zero, 0, 512);
        for (uint32_t s = TAGFS_JOURNAL_ENTRIES_START;
             s < TAGFS_JOURNAL_ENTRIES_START + TAGFS_JOURNAL_ENTRY_COUNT * 2; s++) {
            write_at_sector(disk, s, zero, 512);
        }
    }

    /* ---- Build and write superblock ---- */
    printf("[create_tagfs] Writing superblock...\n");

    uint32_t used_blocks = next_block;
    uint32_t free_blocks = total_blocks - used_blocks;

    TagFSSuperblock sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic                    = TAGFS_MAGIC;
    sb.version                  = TAGFS_VERSION;
    sb.block_size               = TAGFS_BLOCK_SIZE;
    sb.total_blocks             = total_blocks;
    sb.free_blocks              = free_blocks;
    sb.total_files              = (uint32_t)file_count;
    sb.next_file_id             = (uint32_t)(file_count + 1);
    sb.next_tag_id              = (uint32_t)g_tag_count;
    sb.total_tags               = (uint32_t)g_tag_count;

    sb.tag_registry_block       = 0;
    sb.tag_registry_block_count = 1;
    sb.file_table_block         = 1;
    sb.file_table_block_count   = 1;
    sb.metadata_pool_block      = 2;
    sb.metadata_pool_block_count = mpool_block_count;
    sb.block_bitmap_sector       = TAGFS_BITMAP_SECTOR_START;
    sb.block_bitmap_sector_count = bitmap_sectors;
    sb.journal_superblock_sector = TAGFS_JOURNAL_SB_SECTOR;

    sb.fs_created_time          = (uint64_t)time(NULL);
    sb.fs_modified_time         = sb.fs_created_time;
    sb.backup_superblock_sector = TAGFS_BACKUP_SB_SECTOR;

    srand((unsigned int)sb.fs_created_time);
    generate_uuid(sb.fs_uuid);

    /* ---- Boot hints in reserved[] ---- */
    if (kernel_file_index >= 0) {
        FileInfo* kf = &files[kernel_file_index];
        uint32_t kblock  = kf->start_block;
        uint32_t kblocks = kf->block_count;
        uint32_t ksize   = (uint32_t)kf->file_size;

        memcpy(sb.reserved + BOOT_HINT_KERNEL_BLOCK,  &kblock,            4);
        memcpy(sb.reserved + BOOT_HINT_KERNEL_BLOCKS, &kblocks,           4);
        memcpy(sb.reserved + BOOT_HINT_KERNEL_SIZE,   &ksize,             4);
        memcpy(sb.reserved + BOOT_HINT_DATA_START,    &data_start_sector, 4);

        printf("  Boot hints: kernel at block %u (%u blocks, %u bytes), data_start=%u\n",
               kblock, kblocks, ksize, data_start_sector);
    } else {
        printf("  WARNING: No file tagged 'kernel' found — boot hints empty!\n");

        /* Fallback: put data_start anyway */
        memcpy(sb.reserved + BOOT_HINT_DATA_START, &data_start_sector, 4);
    }

    /* Stamp CRC32 and write primary + backup */
    superblock_stamp_crc(&sb);

    if (write_at_sector(disk, TAGFS_SUPERBLOCK_SECTOR, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "Failed to write primary superblock\n");
        fclose(disk); return 1;
    }
    if (write_at_sector(disk, TAGFS_BACKUP_SB_SECTOR, &sb, sizeof(sb)) != 0) {
        fprintf(stderr, "Failed to write backup superblock\n");
        fclose(disk); return 1;
    }

    fclose(disk);

    printf("\n[create_tagfs] TagFS v1 formatting complete!\n");
    printf("  Files:  %d\n", file_count);
    printf("  Tags:   %u\n", g_tag_count);
    printf("  Blocks: %u used / %u total (%u free)\n", used_blocks, total_blocks, free_blocks);

    /* Cleanup */
    for (int i = 0; i < file_count; i++) {
        free(files[i].filepath);
        free(files[i].filename);
        free(files[i].tag_ids);
    }
    free(files);
    for (uint32_t i = 0; i < g_tag_count; i++) {
        free(g_tags[i].key);
        free(g_tags[i].value);
    }
    free(g_tags);

    return 0;
}
