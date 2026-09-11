
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "tagfs_reserved.h"
#include "volume_deed.h"
#include "deed_pen.h"
#include "volume_ledger.h"


#define TAGFS_MAGIC            0x54414746
#define TAGFS_REGISTRY_MAGIC   0x54524547
#define TAGFS_FILETBL_MAGIC    0x54465442
#define TAGFS_MPOOL_MAGIC      0x544D504C
#define JOURNAL_MAGIC          0x4A4F5552

#define TAGFS_VERSION          1
#define TAGFS_BLOCK_SIZE       4096
#define TAGFS_FILE_ACTIVE      (1 << 0)

#define TAGFS_SECTOR_BYTES     512
#define TAGFS_BLOCK_SECTORS    (TAGFS_BLOCK_SIZE / TAGFS_SECTOR_BYTES)

#define DISK_BOOK_SB_MAGIC            0x44425342
#define DISK_BOOK_VERSION             2
#define DISK_BOOK_CAPACITY            512
#define DISK_BOOK_SECTORS_PER_ENTRY   2
#define DISK_BOOK_ENTRY_BLOCKS \
    ((DISK_BOOK_CAPACITY * DISK_BOOK_SECTORS_PER_ENTRY) / TAGFS_BLOCK_SECTORS)

#define DEED_BLOCKS   1
#define DEED_SECTORS  DEED_PEN_SECTORS

#define GROUND_MBR_TYPE_BOXOS      0x7F
#define GROUND_MBR_TYPE_PROTECTIVE 0xEE

#define TAGFS_REGISTRY_DATA_SIZE   4080
#define TAGFS_MPOOL_DATA_SIZE      4080
#define TAGFS_FTABLE_PER_BLOCK     510
#define TAGFS_INVALID_TAG_ID       0xFFFF

#define RECORD_HEADER_SIZE    42
#define RECORD_CRC_OFFSET     40
#define MPOOL_BLOCK_HEADER    16


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
    uint16_t kept;
    uint8_t  reserved[2];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} MetaPoolBlock;


typedef struct {
    char*    key;
    char*    value;
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


static uint16_t intern_tag(const char* key, const char* value) {
    for (uint32_t i = 0; i < g_tag_count; i++) {
        if (strcmp(g_tags[i].key, key) != 0) continue;
        if (value == NULL && g_tags[i].value == NULL) return g_tags[i].tag_id;
        if (value && g_tags[i].value && strcmp(g_tags[i].value, value) == 0)
            return g_tags[i].tag_id;
    }

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

static void seed_reserved_tags(void) {
#define X(k) (void)intern_tag(k, NULL);
    TAGFS_RESERVED_KEYS(X)
#undef X
}

static void extract_stem(const char* filename, char* stem, size_t stem_size) {
    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= stem_size) len = stem_size - 1;
    memcpy(stem, base, len);
    stem[len] = '\0';
}

static int has_tag_id(const uint16_t* ids, uint16_t count, uint16_t id) {
    for (uint16_t i = 0; i < count; i++) {
        if (ids[i] == id) return 1;
    }
    return 0;
}

static void parse_tags(const char* tag_string, const char* filename,
                        uint16_t** out_ids, uint16_t* out_count) {
    uint16_t  ids[256];
    uint16_t  count = 0;

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

        if (!has_tag_id(ids, count, id)) {
            ids[count++] = id;
        }
        token = strtok(NULL, ",");
    }

    *out_count = count;
    *out_ids = malloc(sizeof(uint16_t) * count);
    memcpy(*out_ids, ids, sizeof(uint16_t) * count);
}



static uint64_t g_ground_start;
static uint64_t g_ground_sectors;

static int write_at_sector(FILE* disk, uint64_t vsector, const void* data, size_t size) {
    uint64_t at = (g_ground_start + vsector) * TAGFS_SECTOR_BYTES;
    if (fseek(disk, (long)at, SEEK_SET) != 0) return -1;
    if (fwrite(data, size, 1, disk) != 1) return -1;
    return 0;
}

static int read_at_sector(FILE* disk, uint64_t abs_sector, void* data, size_t size) {
    if (fseek(disk, (long)(abs_sector * TAGFS_SECTOR_BYTES), SEEK_SET) != 0) return -1;
    if (fread(data, size, 1, disk) != 1) return -1;
    return 0;
}

static int write_block(FILE* disk, uint32_t data_start_block, uint32_t block,
                        const void* data) {
    uint64_t vsector = (uint64_t)(data_start_block + block) * TAGFS_BLOCK_SECTORS;
    return write_at_sector(disk, vsector, data, TAGFS_BLOCK_SIZE);
}

static int write_file_data(FILE* disk, uint32_t data_start_block, uint32_t block,
                            FILE* src, uint64_t file_size, uint32_t block_count) {
    uint64_t vsector = (uint64_t)(data_start_block + block) * TAGFS_BLOCK_SECTORS;
    if (fseek(disk, (long)((g_ground_start + vsector) * TAGFS_SECTOR_BYTES), SEEK_SET) != 0)
        return -1;

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


static uint32_t tagfs_crc32(const uint8_t* data, uint32_t len) {
    return DeedPenSum(data, len);
}


#define MBR_TABLE_OFFSET     446u
#define MBR_ENTRY_BYTES      16u
#define MBR_ENTRY_COUNT      4u
#define MBR_ENTRY_TYPE       4u
#define MBR_ENTRY_START_LBA  8u
#define MBR_ENTRY_SECTORS    12u

#define GPT_HEADER_LBA          1u
#define GPT_HEADER_SIZE_OFFSET  0x0Cu
#define GPT_HEADER_CRC_OFFSET   0x10u
#define GPT_ENTRY_LBA_OFFSET    0x48u
#define GPT_ENTRY_COUNT_OFFSET  0x50u
#define GPT_ENTRY_BYTES_OFFSET  0x54u
#define GPT_ENTRY_CRC_OFFSET    0x58u
#define GPT_ENTRY_TYPE_OFFSET   0u
#define GPT_ENTRY_FIRST_OFFSET  0x20u
#define GPT_ENTRY_LAST_OFFSET   0x28u
#define GPT_HEADER_MIN_BYTES    92u
#define GPT_ENTRY_MIN_BYTES     128u
#define GPT_ARRAY_MAX_BYTES     (128u * 1024u)

static const uint8_t g_boxos_type_guid[16] = {
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb
};

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t* p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int ground_from_gpt(FILE* disk) {
    uint8_t header[TAGFS_SECTOR_BYTES];
    if (read_at_sector(disk, GPT_HEADER_LBA, header, sizeof(header)) != 0) {
        fprintf(stderr, "This image says it is a GPT and has no header behind it\n");
        return -1;
    }
    if (memcmp(header, "EFI PART", 8) != 0) {
        fprintf(stderr, "A protective MBR with no GPT behind it\n");
        return -1;
    }

    uint32_t header_bytes = le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES || header_bytes > TAGFS_SECTOR_BYTES) {
        fprintf(stderr, "Its GPT header states a length of %u, which cannot be one\n",
                header_bytes);
        return -1;
    }

    uint8_t probe[TAGFS_SECTOR_BYTES];
    memcpy(probe, header, header_bytes);
    memset(probe + GPT_HEADER_CRC_OFFSET, 0, 4);
    if (tagfs_crc32(probe, header_bytes) != le32(header + GPT_HEADER_CRC_OFFSET)) {
        fprintf(stderr, "Its GPT header does not match its own checksum\n");
        return -1;
    }

    uint64_t entry_lba   = le64(header + GPT_ENTRY_LBA_OFFSET);
    uint32_t entry_count = le32(header + GPT_ENTRY_COUNT_OFFSET);
    uint32_t entry_bytes = le32(header + GPT_ENTRY_BYTES_OFFSET);

    if (entry_bytes < GPT_ENTRY_MIN_BYTES || entry_count == 0 ||
        entry_bytes > GPT_ARRAY_MAX_BYTES ||
        entry_count > GPT_ARRAY_MAX_BYTES / entry_bytes) {
        fprintf(stderr, "Its GPT states %u entries of %u bytes, which is not a table\n",
                entry_count, entry_bytes);
        return -1;
    }

    uint32_t array_bytes = entry_count * entry_bytes;
    uint8_t* array = calloc(1, array_bytes);
    if (!array) { fprintf(stderr, "No memory for its GPT entries\n"); return -1; }

    int rc = -1;
    if (read_at_sector(disk, entry_lba, array, array_bytes) != 0) {
        fprintf(stderr, "Could not read its GPT entries\n");
        goto done;
    }
    if (tagfs_crc32(array, array_bytes) != le32(header + GPT_ENTRY_CRC_OFFSET)) {
        fprintf(stderr, "Its GPT entries do not match their own checksum\n");
        goto done;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t* e = array + (uint64_t)i * entry_bytes;
        if (memcmp(e + GPT_ENTRY_TYPE_OFFSET, g_boxos_type_guid, 16) != 0) continue;

        uint64_t first = le64(e + GPT_ENTRY_FIRST_OFFSET);
        uint64_t last  = le64(e + GPT_ENTRY_LAST_OFFSET);
        if (last < first) continue;

        g_ground_start   = first;
        g_ground_sectors = last - first + 1;
        printf("  Ground:        sectors %llu..%llu, from GPT entry %u\n",
               (unsigned long long)first, (unsigned long long)last, i);
        rc = 0;
        goto done;
    }
    fprintf(stderr, "Its GPT has no BoxOS partition in it\n");

done:
    free(array);
    return rc;
}

static int survey_ground(FILE* disk) {
    uint8_t sector0[TAGFS_SECTOR_BYTES];
    if (read_at_sector(disk, 0, sector0, sizeof(sector0)) != 0) {
        fprintf(stderr, "Could not read sector 0 of the image\n");
        return -1;
    }
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        fprintf(stderr, "No partition table on this image — nothing has claimed "
                        "ground for BoxOS, so there is nowhere to put a volume\n");
        return -1;
    }

    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES + MBR_ENTRY_TYPE] ==
            GROUND_MBR_TYPE_PROTECTIVE) {
            return ground_from_gpt(disk);
        }
    }

    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t* e = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        if (e[MBR_ENTRY_TYPE] != GROUND_MBR_TYPE_BOXOS) continue;

        uint64_t start = le32(e + MBR_ENTRY_START_LBA);
        uint64_t count = le32(e + MBR_ENTRY_SECTORS);
        if (count == 0) continue;

        g_ground_start   = start;
        g_ground_sectors = count;
        printf("  Ground:        sectors %llu..%llu, from MBR entry %u\n",
               (unsigned long long)start,
               (unsigned long long)(start + count - 1), i);
        return 0;
    }

    fprintf(stderr, "This image has a partition table with no BoxOS partition in it\n");
    return -1;
}


typedef struct {
    uint32_t volume_blocks;
    uint32_t deed_block;
    uint32_t ledger_block;
    uint32_t disk_book_block;
    uint32_t disk_book_blocks;
    uint32_t bitmap_block;
    uint32_t bitmap_blocks;
    uint32_t data_block;
    uint32_t data_blocks;
    uint64_t tail_sector;
} VolumeShape;

#define BITMAP_BLOCK_COVERS  ((uint32_t)(TAGFS_BLOCK_SIZE * 8))

#define GROWTH_CEILING_BLOCKS \
    ((uint32_t)((64ull * 1024 * 1024 * 1024) / TAGFS_BLOCK_SIZE))
#define GROWTH_CEILING_BITMAP_BLOCKS \
    ((GROWTH_CEILING_BLOCKS + BITMAP_BLOCK_COVERS - 1) / BITMAP_BLOCK_COVERS)

static int compute_layout(uint64_t ground_sectors, VolumeShape* out) {
    memset(out, 0, sizeof(*out));

    out->volume_blocks    = (uint32_t)(ground_sectors / TAGFS_BLOCK_SECTORS);
    out->deed_block       = 0;
    out->ledger_block     = DEED_BLOCKS;
    out->disk_book_block  = out->ledger_block + VOLUME_LEDGER_COPIES;
    out->disk_book_blocks = 2 + DISK_BOOK_ENTRY_BLOCKS;
    out->bitmap_block     = out->disk_book_block + out->disk_book_blocks;

    uint32_t bitmap_blocks = GROWTH_CEILING_BITMAP_BLOCKS;
    for (int i = 0; i < 32; i++) {
        uint32_t data_block = out->bitmap_block + bitmap_blocks;
        if (out->volume_blocks <= data_block + DEED_BLOCKS) return -1;
        uint32_t data_blocks = out->volume_blocks - data_block - DEED_BLOCKS;
        uint32_t need = ((data_blocks + 7) / 8 + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
        if (need <= bitmap_blocks) break;
        bitmap_blocks = need;
    }

    out->bitmap_blocks = bitmap_blocks;
    out->data_block    = out->bitmap_block + bitmap_blocks;
    if (out->volume_blocks <= out->data_block + DEED_BLOCKS) return -1;
    out->data_blocks   = out->volume_blocks - out->data_block - DEED_BLOCKS;

    if ((uint64_t)out->data_blocks >
        (uint64_t)out->bitmap_blocks * BITMAP_BLOCK_COVERS) return -1;

    out->tail_sector = (uint64_t)(out->volume_blocks - DEED_BLOCKS) * TAGFS_BLOCK_SECTORS;
    return 0;
}


static int write_deed(FILE* disk, uint32_t role, uint64_t vsector,
                      const uint8_t uuid[16], const VolumeShape* shape,
                      const VolumeBoot* boot, uint32_t mpool_blocks,
                      uint32_t ftable_blocks, uint32_t registry_blocks) {
    uint8_t block[TAGFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    VolumeGeometry geo;
    memset(&geo, 0, sizeof(geo));
    geo.logical_bytes  = TAGFS_SECTOR_BYTES;
    geo.physical_bytes = TAGFS_BLOCK_SIZE;
    geo.grain_bytes    = 1024u * 1024u;
    geo.block_bytes    = TAGFS_BLOCK_SIZE;

    VolumeLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.total_blocks         = shape->volume_blocks;
    layout.state_block          = shape->ledger_block;
    layout.state_blocks         = VOLUME_LEDGER_COPIES;
    layout.tag_registry_block   = shape->data_block + 0;
    layout.file_table_block     = shape->data_block + 1;
    layout.metadata_pool_block  = shape->data_block + 2;
    layout.tag_registry_blocks  = registry_blocks;
    layout.file_table_blocks    = ftable_blocks;
    layout.metadata_pool_blocks = mpool_blocks;
    layout.block_bitmap_block   = shape->bitmap_block;
    layout.block_bitmap_blocks  = shape->bitmap_blocks;
    layout.disk_book_block      = shape->disk_book_block;
    layout.disk_book_blocks     = shape->disk_book_blocks;
    layout.data_block           = shape->data_block;
    layout.data_blocks          = shape->data_blocks;

    VolumeBorn born;
    memset(&born, 0, sizeof(born));
    born.created_unix = (uint64_t)time(NULL);
    memcpy(born.maker, "create_tagfs", 12);

    DeedPen pen;
    DeedPenOpen(&pen, block, sizeof(block));
    if (DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo,    sizeof(geo))    != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_LAYOUT,   &layout, sizeof(layout)) != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BORN,     &born,   sizeof(born))   != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BOOT,     boot,    sizeof(*boot))  != 0) {
        fprintf(stderr, "The stamps of this Deed do not fit the block it lives in\n");
        return -1;
    }

    DeedPenSetUuid(&pen, uuid);
    DeedPenSetGround(&pen,
                     (uint64_t)shape->volume_blocks * TAGFS_BLOCK_SECTORS,
                     shape->tail_sector);
    DeedPenSetRole(&pen, role);
    DeedPenSeal(&pen);

    return write_at_sector(disk, vsector, block, sizeof(block));
}


static int write_ledger(FILE* disk, const VolumeShape* shape,
                        const VolumeLedger* src) {
    static const uint8_t magic[8] = {
        VOLUME_LEDGER_MAGIC_0, VOLUME_LEDGER_MAGIC_1, VOLUME_LEDGER_MAGIC_2,
        VOLUME_LEDGER_MAGIC_3, VOLUME_LEDGER_MAGIC_4, VOLUME_LEDGER_MAGIC_5,
        VOLUME_LEDGER_MAGIC_6, VOLUME_LEDGER_MAGIC_7
    };

    VolumeLedger led = *src;
    memcpy(led.magic, magic, sizeof(magic));
    led.bytes = sizeof(VolumeLedger);
    led.crc32 = 0;
    led.crc32 = tagfs_crc32((const uint8_t*)&led, sizeof(led));

    for (uint32_t copy = 0; copy < VOLUME_LEDGER_COPIES; copy++) {
        uint8_t block[TAGFS_BLOCK_SIZE];
        memset(block, 0, sizeof(block));
        memcpy(block, &led, sizeof(led));

        uint64_t vsector = (uint64_t)(shape->ledger_block + copy) * TAGFS_BLOCK_SECTORS;
        if (write_at_sector(disk, vsector, block, sizeof(block)) != 0) return -1;
    }
    return 0;
}


static uint32_t registry_record_size(const TagEntry* t) {
    uint32_t key_len   = (uint32_t)strlen(t->key);
    uint32_t value_len = t->value ? (uint32_t)strlen(t->value) : 0;
    return 2 + 1 + 1 + 2 + key_len + value_len;
}

static int write_registry_chain(FILE* disk, uint32_t data_start_block,
                                uint32_t* next_free, uint32_t total_blocks,
                                uint32_t* out_blocks) {
    TagRegistryBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic = TAGFS_REGISTRY_MAGIC;

    uint32_t here   = 0;
    uint32_t blocks = 1;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_tag_count; i++) {
        uint8_t  key_len   = (uint8_t)strlen(g_tags[i].key);
        uint16_t value_len = g_tags[i].value ? (uint16_t)strlen(g_tags[i].value) : 0;
        uint8_t  flags     = g_tags[i].value ? 1 : 0;

        uint32_t record_size = registry_record_size(&g_tags[i]);

        if (record_size > TAGFS_REGISTRY_DATA_SIZE) {
            fprintf(stderr, "The tag '%s' needs %u bytes and a registry block "
                            "holds %u — it cannot be written\n",
                    g_tags[i].key, record_size, TAGFS_REGISTRY_DATA_SIZE);
            return -1;
        }

        if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
            uint32_t next = (*next_free)++;
            if (next >= total_blocks) {
                fprintf(stderr, "Not enough blocks for the tag registry chain\n");
                return -1;
            }

            blk.next_block = next;
            if (write_block(disk, data_start_block, here, &blk) != 0) {
                fprintf(stderr, "Failed to write tag registry block %u\n", here);
                return -1;
            }

            here = next;
            memset(&blk, 0, sizeof(blk));
            blk.magic = TAGFS_REGISTRY_MAGIC;
            offset = 0;
            blocks++;
        }

        uint8_t* p = blk.data + offset;

        memcpy(p, &g_tags[i].tag_id, 2); p += 2;
        *p++ = flags;
        *p++ = key_len;
        memcpy(p, &value_len, 2); p += 2;
        memcpy(p, g_tags[i].key, key_len); p += key_len;
        if (value_len > 0) {
            memcpy(p, g_tags[i].value, value_len);
        }

        offset += record_size;
        blk.entry_count++;
        blk.used_bytes = (uint16_t)offset;
    }

    if (write_block(disk, data_start_block, here, &blk) != 0) {
        fprintf(stderr, "Failed to write tag registry block %u\n", here);
        return -1;
    }

    *out_blocks = blocks;
    return 0;
}

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


static uint32_t pack_metadata_record(uint8_t* buf, const FileInfo* fi) {
    const char* fname = fi->filename;
    uint16_t name_len = (uint16_t)strlen(fname);
    uint16_t extent_count = 1;

    uint16_t record_len = (uint16_t)(RECORD_HEADER_SIZE
                          + fi->tag_count * sizeof(uint16_t)
                          + extent_count * sizeof(FileExtent)
                          + name_len);

    uint32_t pos = 0;

    memcpy(buf + pos, &record_len, 2);            pos += 2;
    memcpy(buf + pos, &fi->file_id, 4);           pos += 4;
    uint32_t flags = TAGFS_FILE_ACTIVE;
    memcpy(buf + pos, &flags, 4);                 pos += 4;
    memcpy(buf + pos, &fi->file_size, 8);         pos += 8;
    uint64_t now = (uint64_t)time(NULL);
    memcpy(buf + pos, &now, 8);                   pos += 8;
    memcpy(buf + pos, &now, 8);                   pos += 8;
    memcpy(buf + pos, &fi->tag_count, 2);         pos += 2;
    memcpy(buf + pos, &extent_count, 2);          pos += 2;
    memcpy(buf + pos, &name_len, 2);              pos += 2;
    uint16_t zero_crc = 0;
    memcpy(buf + pos, &zero_crc, 2);              pos += 2;

    if (fi->tag_count > 0) {
        memcpy(buf + pos, fi->tag_ids, fi->tag_count * 2);
        pos += fi->tag_count * 2;
    }

    FileExtent ext;
    ext.start_block = fi->start_block;
    ext.block_count = (uint16_t)fi->block_count;
    memcpy(buf + pos, &ext, sizeof(FileExtent));
    pos += sizeof(FileExtent);

    memcpy(buf + pos, fname, name_len);
    pos += name_len;

    uint16_t crc = meta_crc16(buf, (uint32_t)record_len);
    memcpy(buf + RECORD_CRC_OFFSET, &crc, 2);

    return (uint32_t)record_len;
}


static void generate_uuid(uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) uuid[i] = rand() & 0xFF;
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}



#define GROUND_WORKING_ROOM_BLOCKS   ((64u * 1024u * 1024u) / TAGFS_BLOCK_SIZE)

#define GROUND_METADATA_SLACK_BLOCKS 8u

#define GROUND_GRAIN_SECTORS         2048u

static int say_the_ground(int pair_count, char* argv[], int first) {
    uint64_t data_blocks = 3;
    data_blocks += GROUND_METADATA_SLACK_BLOCKS;

    data_blocks += ((uint64_t)pair_count + 1u + TAGFS_FTABLE_PER_BLOCK - 1u)
                   / TAGFS_FTABLE_PER_BLOCK;

    for (int i = 0; i < pair_count; i++) {
        const char* path = argv[first + i * 2];
        FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "create_tagfs --ground: cannot read %s\n", path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size < 0) size = 0;
        data_blocks += ((uint64_t)size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    }

    seed_reserved_tags();
    for (int i = 0; i < pair_count; i++) {
        uint16_t* ids   = NULL;
        uint16_t  count = 0;
        parse_tags(argv[first + i * 2 + 1], argv[first + i * 2], &ids, &count);
        free(ids);
    }
    {
        uint64_t reg_blocks = 1;
        uint32_t offset     = 0;
        for (uint32_t i = 0; i < g_tag_count; i++) {
            uint32_t record_size = registry_record_size(&g_tags[i]);
            if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
                reg_blocks++;
                offset = 0;
            }
            offset += record_size;
        }
        data_blocks += reg_blocks - 1;
    }

    data_blocks += GROUND_WORKING_ROOM_BLOCKS;

    uint64_t ground = (data_blocks + 64) * TAGFS_BLOCK_SECTORS;
    ground = ((ground + GROUND_GRAIN_SECTORS - 1) / GROUND_GRAIN_SECTORS) * GROUND_GRAIN_SECTORS;

    for (;;) {
        VolumeShape shape;
        if (compute_layout(ground, &shape) == 0 && shape.data_blocks >= data_blocks)
            break;
        ground += GROUND_GRAIN_SECTORS;
    }

    printf("%llu\n", (unsigned long long)ground);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        if ((argc - 2) % 2 != 0) {
            fprintf(stderr, "Usage: %s --ground [<file> <tags>] ...\n", argv[0]);
            return 1;
        }
        return say_the_ground((argc - 2) / 2, argv, 2);
    }

    if (argc < 2 || (argc > 2 && (argc - 2) % 2 != 0)) {
        fprintf(stderr, "Usage: %s <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "       %s --ground [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
        fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
        fprintf(stderr, "type GUID in a GPT). Nothing outside that run is touched.\n");
        fprintf(stderr, "The file whose name stems to 'kernel' is the one the Deed\n");
        fprintf(stderr, "points a loader at.\n");
        return 1;
    }

    const char* disk_path = argv[1];
    int file_count = (argc - 2) / 2;

    FILE* disk = fopen(disk_path, "r+b");
    if (!disk) { perror("Failed to open disk image"); return 1; }

    fseek(disk, 0, SEEK_END);
    long disk_size = ftell(disk);

    printf("[create_tagfs] Making a BoxOS volume on %s (%lu bytes)\n",
           disk_path, disk_size);

    if (survey_ground(disk) != 0) { fclose(disk); return 1; }

    if ((long)((g_ground_start + g_ground_sectors) * TAGFS_SECTOR_BYTES) > disk_size) {
        fprintf(stderr, "The BoxOS partition runs to sector %llu and the image "
                        "ends at %llu — it was copied short\n",
                (unsigned long long)(g_ground_start + g_ground_sectors),
                (unsigned long long)(disk_size / TAGFS_SECTOR_BYTES));
        fclose(disk); return 1;
    }

    VolumeShape shape;
    if (compute_layout(g_ground_sectors, &shape) != 0) {
        fprintf(stderr,
                "The ground claimed for BoxOS is %llu sectors, and a volume's "
                "front matter alone needs more than that\n",
                (unsigned long long)g_ground_sectors);
        fclose(disk);
        return 1;
    }
    uint32_t total_blocks      = shape.data_blocks;
    uint32_t data_start_block  = shape.data_block;

    printf("  Deed:          block %u, far copy at sector %llu\n",
           shape.deed_block, (unsigned long long)shape.tail_sector);
    printf("  Ledger:        blocks %u..%u\n",
           shape.ledger_block, shape.ledger_block + VOLUME_LEDGER_COPIES - 1);
    printf("  DiskBook:      blocks %u..%u (head, backup, %u record blocks)\n",
           shape.disk_book_block,
           shape.disk_book_block + shape.disk_book_blocks - 1,
           DISK_BOOK_ENTRY_BLOCKS);
    printf("  Block bitmap:  blocks %u..%u\n",
           shape.bitmap_block, shape.bitmap_block + shape.bitmap_blocks - 1);
    printf("  Data:          blocks %u..%u — %u of them (%u MiB)\n",
           shape.data_block, shape.data_block + shape.data_blocks - 1,
           shape.data_blocks, (shape.data_blocks * 4) / 1024);
    printf("  Reserved:      data block 0 (registry), 1 (file table), 2 (metadata pool)\n");

    FileInfo* files = NULL;
    if (file_count > 0) {
        files = calloc(file_count, sizeof(FileInfo));
        if (!files) { fprintf(stderr, "calloc failed\n"); fclose(disk); return 1; }
    }

    seed_reserved_tags();

    uint32_t next_block = 3;
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
        files[i].file_id     = (uint32_t)(i + 1);
        files[i].block_count = (uint32_t)((files[i].file_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
        if (files[i].block_count == 0) files[i].block_count = 1;
        files[i].start_block = next_block;

        parse_tags(tags_str, files[i].filename, &files[i].tag_ids, &files[i].tag_count);

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

    printf("\n[create_tagfs] Writing tag registry...\n");
    uint32_t reg_blocks = 1;
    if (write_registry_chain(disk, data_start_block, &next_block,
                             total_blocks, &reg_blocks) != 0) {
        fclose(disk); return 1;
    }
    if (reg_blocks > 1) {
        printf("  Tag registry: %u tags over %u blocks\n", g_tag_count, reg_blocks);
    }

    printf("[create_tagfs] Writing file data...\n");
    for (int i = 0; i < file_count; i++) {
        FILE* f = fopen(files[i].filepath, "rb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for data write\n", files[i].filepath);
            fclose(disk); return 1;
        }
        if (write_file_data(disk, data_start_block, files[i].start_block,
                            f, files[i].file_size, files[i].block_count) != 0) {
            fprintf(stderr, "Failed to write data for %s\n", files[i].filename);
            fclose(f); fclose(disk); return 1;
        }
        fclose(f);
    }

    printf("[create_tagfs] Writing metadata pool...\n");
    MetaPoolBlock mpool;
    memset(&mpool, 0, sizeof(MetaPoolBlock));
    mpool.magic = TAGFS_MPOOL_MAGIC;

    uint32_t current_mpool_block = 2;
    uint32_t mpool_block_count   = 1;

    uint32_t ftable_entries = file_count > 0
                            ? (uint32_t)files[file_count - 1].file_id + 1u
                            : 0u;
    FileTableEntry* ftable_all = calloc(ftable_entries ? ftable_entries : 1,
                                        sizeof(FileTableEntry));
    if (!ftable_all) {
        fprintf(stderr, "calloc failed for the file table\n");
        fclose(disk); return 1;
    }

    for (int i = 0; i < file_count; i++) {
        uint8_t record_buf[4096];
        uint32_t record_size = pack_metadata_record(record_buf, &files[i]);

        if (record_size > TAGFS_MPOOL_DATA_SIZE) {
            fprintf(stderr, "Metadata record too large for %s (%u bytes)\n",
                    files[i].filename, record_size);
            fclose(disk); return 1;
        }

        if (mpool.used_bytes + record_size > TAGFS_MPOOL_DATA_SIZE) {
            uint32_t new_mpool_block = next_block++;
            if (new_mpool_block >= total_blocks) {
                fprintf(stderr, "Not enough blocks for metadata pool chain\n");
                fclose(disk); return 1;
            }

            mpool.next_block = new_mpool_block;
            if (write_block(disk, data_start_block, current_mpool_block, &mpool) != 0) {
                fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
                free(ftable_all); fclose(disk); return 1;
            }
            printf("  Metadata pool: block %u full (%u records, %u bytes), chaining to block %u\n",
                   current_mpool_block, mpool.record_count, mpool.used_bytes, new_mpool_block);

            current_mpool_block = new_mpool_block;
            memset(&mpool, 0, sizeof(MetaPoolBlock));
            mpool.magic = TAGFS_MPOOL_MAGIC;
            mpool_block_count++;
        }

        uint32_t meta_offset = MPOOL_BLOCK_HEADER + mpool.used_bytes;
        memcpy(mpool.payload + mpool.used_bytes, record_buf, record_size);
        mpool.used_bytes += (uint16_t)record_size;
        mpool.record_count++;
        mpool.kept++;

        uint32_t fid = files[i].file_id;
        if (fid < ftable_entries) {
            ftable_all[fid].meta_block  = current_mpool_block;
            ftable_all[fid].meta_offset = meta_offset;
        }
    }

    if (write_block(disk, data_start_block, current_mpool_block, &mpool) != 0) {
        fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
        free(ftable_all); fclose(disk); return 1;
    }
    if (mpool_block_count > 1) {
        printf("  Metadata pool: %u blocks total\n", mpool_block_count);
    }

    printf("[create_tagfs] Writing file table...\n");
    uint32_t ftable_blocks =
        (ftable_entries + TAGFS_FTABLE_PER_BLOCK - 1) / TAGFS_FTABLE_PER_BLOCK;
    if (ftable_blocks == 0) ftable_blocks = 1;

    {
        uint32_t here = 1;
        for (uint32_t bi = 0; bi < ftable_blocks; bi++) {
            uint32_t taken = bi * TAGFS_FTABLE_PER_BLOCK;
            uint32_t count = ftable_entries > taken ? ftable_entries - taken : 0;
            if (count > TAGFS_FTABLE_PER_BLOCK) count = TAGFS_FTABLE_PER_BLOCK;

            uint32_t next = 0;
            if (bi + 1 < ftable_blocks) {
                next = next_block++;
                if (next >= total_blocks) {
                    fprintf(stderr, "Not enough blocks for the file table chain\n");
                    free(ftable_all); fclose(disk); return 1;
                }
            }

            FileTableBlock ftable;
            memset(&ftable, 0, sizeof(FileTableBlock));
            ftable.magic       = TAGFS_FILETBL_MAGIC;
            ftable.next_block  = next;
            ftable.entry_count = count;
            for (uint32_t i = 0; i < count; i++) {
                ftable.entries[i] = ftable_all[taken + i];
            }

            if (write_block(disk, data_start_block, here, &ftable) != 0) {
                fprintf(stderr, "Failed to write file table block %u\n", here);
                free(ftable_all); fclose(disk); return 1;
            }
            here = next;
        }
        if (ftable_blocks > 1) {
            printf("  File table: %u entries over %u blocks\n",
                   ftable_entries, ftable_blocks);
        }
    }
    free(ftable_all);

    printf("[create_tagfs] Writing block bitmap...\n");
    uint32_t bitmap_buf_size = shape.bitmap_blocks * TAGFS_BLOCK_SIZE;
    uint8_t* bitmap = calloc(1, bitmap_buf_size);
    if (!bitmap) { fprintf(stderr, "calloc failed for bitmap\n"); fclose(disk); return 1; }

    for (uint32_t b = 0; b < next_block && b < total_blocks; b++) {
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    }

    if (write_at_sector(disk, (uint64_t)shape.bitmap_block * TAGFS_BLOCK_SECTORS,
                        bitmap, bitmap_buf_size) != 0) {
        fprintf(stderr, "Failed to write block bitmap\n");
        free(bitmap); fclose(disk); return 1;
    }
    free(bitmap);

    printf("[create_tagfs] Writing DiskBook...\n");
    {
        uint8_t dbuf[TAGFS_SECTOR_BYTES];
        memset(dbuf, 0, sizeof(dbuf));
        uint32_t dmag   = DISK_BOOK_SB_MAGIC;
        uint32_t dver   = DISK_BOOK_VERSION;
        uint64_t dstart = (uint64_t)(shape.disk_book_block + 2) * TAGFS_BLOCK_SECTORS;
        uint32_t dcap   = DISK_BOOK_CAPACITY;
        uint32_t dcount = 0;
        memcpy(dbuf + 0,  &dmag,   4);
        memcpy(dbuf + 4,  &dver,   4);
        memcpy(dbuf + 8,  &dstart, 8);
        memcpy(dbuf + 16, &dcap,   4);
        memcpy(dbuf + 20, &dcount, 4);
        uint32_t dcrc = tagfs_crc32(dbuf, sizeof(dbuf));
        memcpy(dbuf + 32, &dcrc, 4);

        write_at_sector(disk, (uint64_t)shape.disk_book_block * TAGFS_BLOCK_SECTORS,
                        dbuf, sizeof(dbuf));
        write_at_sector(disk, (uint64_t)(shape.disk_book_block + 1) * TAGFS_BLOCK_SECTORS,
                        dbuf, sizeof(dbuf));

        uint8_t zero[TAGFS_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        for (uint32_t b = 0; b < DISK_BOOK_ENTRY_BLOCKS; b++) {
            write_at_sector(disk,
                            (uint64_t)(shape.disk_book_block + 2 + b) * TAGFS_BLOCK_SECTORS,
                            zero, sizeof(zero));
        }
    }

    printf("[create_tagfs] Writing the Ledger...\n");

    uint32_t used_blocks = next_block;
    uint32_t free_blocks = total_blocks - used_blocks;

    uint64_t made_at = (uint64_t)time(NULL);
    srand((unsigned int)made_at);

    uint8_t volume_uuid[16];
    generate_uuid(volume_uuid);

    VolumeLedger led;
    memset(&led, 0, sizeof(led));
    led.seq          = 1;
    led.written_unix = made_at;
    led.free_blocks  = free_blocks;
    led.total_files  = (uint64_t)file_count;
    led.next_file_id = (uint32_t)(file_count + 1);
    led.next_tag_id  = g_tag_count;
    led.total_tags   = g_tag_count;

    if (write_ledger(disk, &shape, &led) != 0) {
        fprintf(stderr, "Failed to write the Ledger\n");
        fclose(disk); return 1;
    }

    printf("[create_tagfs] Writing the Deed...\n");

    VolumeBoot boot;
    memset(&boot, 0, sizeof(boot));
    if (kernel_file_index >= 0) {
        FileInfo* kf = &files[kernel_file_index];
        boot.kernel_block  = shape.data_block + kf->start_block;
        boot.kernel_blocks = kf->block_count;
        boot.kernel_bytes  = (uint32_t)kf->file_size;
        printf("  Boot:          kernel at volume block %u (%u blocks, %u bytes)\n",
               boot.kernel_block, boot.kernel_blocks, boot.kernel_bytes);
    } else {
        printf("  WARNING: nothing here is tagged 'kernel' — this volume carries "
               "no kernel and no loader will boot from it\n");
    }

    if (write_deed(disk, VOLUME_DEED_ROLE_HEAD, 0,
                   volume_uuid, &shape, &boot, mpool_block_count,
                   ftable_blocks, reg_blocks) != 0) {
        fprintf(stderr, "Failed to write the Deed\n");
        fclose(disk); return 1;
    }
    if (write_deed(disk, VOLUME_DEED_ROLE_TAIL, shape.tail_sector,
                   volume_uuid, &shape, &boot, mpool_block_count,
                   ftable_blocks, reg_blocks) != 0) {
        fprintf(stderr, "Failed to write the far copy of the Deed\n");
        fclose(disk); return 1;
    }

    fclose(disk);

    printf("\n[create_tagfs] The volume is made.\n");
    printf("  Files:  %d\n", file_count);
    printf("  Tags:   %u\n", g_tag_count);
    printf("  Blocks: %u used / %u data (%u free)\n", used_blocks, total_blocks, free_blocks);

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