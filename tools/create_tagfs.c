/*
 * create_tagfs.c — Make a BoxOS volume on ground somebody has claimed for it.
 *
 * ‼ IT DOES NOT DECIDE WHERE THE VOLUME GOES
 *
 * It reads the medium's partition table, exactly as the kernel does, and lays
 * the volume down on the run of sectors claimed for BoxOS. So the tool that
 * writes a volume and the kernel that mounts it answer "where is it?" the same
 * way, out of the same bytes, instead of both being told the same number by a
 * build script and drifting apart the first time one of them is edited.
 *
 * That is what replaced sector 1034 — an absolute address the tool, the kernel
 * twice, and the UEFI loader each spelled out for themselves.
 *
 * ‼ EVERYTHING IT WRITES IS COUNTED FROM THE START OF THAT GROUND
 *
 *   block 0            the Deed — the volume's title to its ground
 *   block 1, 2         the Ledger, two copies, written alternately
 *   block 3, 4         the DiskBook's own head and its backup
 *   block 5 +          the DiskBook's records
 *   then               the block bitmap, covering exactly the data run
 *   then               the data: registry, file table, metadata pool, files
 *   last block         the Deed again, so the far end proves the volume is
 *                      all there
 *
 * Nothing shares a 4096-byte physical block with its own backup, which is what
 * the old layout did with the superblock, its copy, and the journal head — all
 * three inside sectors 1032..1039, one failed erase away from going together.
 *
 * Usage: create_tagfs <disk_image> [<file> <tags>] ...
 *        create_tagfs --ground [<file> <tags>] ...
 *        create_tagfs --frame-only <disk_image> [<file> <tags>] ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "buildplate_format.h"   /* the verdict this volume copies out of the kernel it packs */

#include "tagfs_reserved.h"  /* shared reserved-tag vocabulary (-I src/include) */
#include "volume_deed.h"     /* the title; types come from <stdint.h> above    */
#include "deed_pen.h"        /* and the one pen it is written with              */
#include "volume_ledger.h"   /* and what is true of the volume today           */

/* ====================================================================
 * Constants
 *
 * The format's own come from the headers the kernel reads it with: the
 * block and its sectors from volume_deed.h, the chains' magics from
 * boxos_magic.h. The rest below are copies of the kernel's
 * tagfs_constants.h and disk_book.h, which a host tool cannot include —
 * they take the kernel's integer types — and must match them.
 * ==================================================================== */

#include "boxos_magic.h"     /* TAGFS_REGISTRY_MAGIC, TAGFS_FILETBL_MAGIC, TAGFS_MPOOL_MAGIC */

#define TAGFS_BLOCK_SIZE       VOLUME_BLOCK_BYTES
#define TAGFS_SECTOR_BYTES     VOLUME_DEED_SECTOR_BYTES
#define TAGFS_BLOCK_SECTORS    VOLUME_BLOCK_SECTORS

#define TAGFS_FILE_ACTIVE      (1 << 0)

/* The DiskBook's on-disk geometry, from the kernel's disk_book.h. Its head and
 * the copy of its head get a block each — they were one sector apart, which on
 * every medium in use is the same physical block. */
#define DISK_BOOK_SB_MAGIC            0x44425342  /* "DBSB" */
#define DISK_BOOK_VERSION             2
#define DISK_BOOK_CAPACITY            512
#define DISK_BOOK_SECTORS_PER_ENTRY   2
#define DISK_BOOK_ENTRY_BLOCKS \
    ((DISK_BOOK_CAPACITY * DISK_BOOK_SECTORS_PER_ENTRY) / TAGFS_BLOCK_SECTORS)

/* A Deed occupies one block wherever it sits. The sector count is the pen's,
 * so the tool and the kernel cannot come to disagree about how much of the
 * ground a Deed takes. */
#define DEED_BLOCKS   1
#define DEED_SECTORS  DEED_PEN_SECTORS

/* What a BoxOS partition looks like from the outside. The MBR type byte and
 * the GPT type GUID are the same two constants the kernel's ground.c reads;
 * they are permanent and are never reissued. */
#define GROUND_MBR_TYPE_BOXOS      0x7F
#define GROUND_MBR_TYPE_PROTECTIVE 0xEE

#define TAGFS_REGISTRY_DATA_SIZE   4080
#define TAGFS_MPOOL_DATA_SIZE      4080
#define TAGFS_FTABLE_PER_BLOCK     510
#define TAGFS_INVALID_TAG_ID       0xFFFF

#define RECORD_HEADER_SIZE    42   /* metadata pool record header (40 fields + 2 CRC16) */
#define RECORD_CRC_OFFSET     40   /* CRC16 stored at bytes [40..41] of packed record */
#define MPOOL_BLOCK_HEADER    16   /* MetaPoolBlock header before payload */
#define INTEGRITY_DIGESTS_PER_BLOCK  (TAGFS_BLOCK_SIZE / 8)   /* integrity.c ENTRIES_PER_BLOCK */

/* ====================================================================
 * On-disk structures — must match kernel's tagfs.h exactly
 * ==================================================================== */

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

/* `kept` sits in two of the four bytes this header set aside and never used, so
 * the block is the same 4096 bytes it always was. It pairs with record_count —
 * one counts what was ever written here, the other what is still held. Nothing
 * is retired at build time, so on a fresh image the two are equal, and the
 * kernel counts the records rather than believing either of them. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint16_t kept;
    uint8_t  reserved[2];
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

/* Intern the reserved vocabulary first so it claims tag_ids 0..11 — the same
 * on-disk ID contract the kernel's format-seed produces. The kernel restores
 * each tag at its stored id (tag_registry.c RegistryAdopt), so the
 * auth-privilege keys (god/stopped/bypass/network) land at ids < 64 and their
 * (1ULL<<id) masks are non-zero. Reserved keys that a file also carries dedup
 * back onto these low ids; genuinely new file tags start at id 12. */
static void seed_reserved_tags(void) {
#define X(k) (void)intern_tag(k, NULL);
    TAGFS_RESERVED_KEYS(X)
#undef X
}

/* ==================================================================
 * THE SHIPMENT'S PEDIGREE
 *
 * The kernel carries a plate saying which tree built it. It cannot say
 * anything about the other sixty-nine files on the volume, and a machine boots
 * all seventy. So the volume gets its own stamp: one number over every file
 * packed, in pack order, contents and all — and the kernel's verdict COPIED
 * out of the plate as the kernel goes by, so the two halves cannot hold two
 * opinions. See VolumePacked in volume_deed.h.
 * ================================================================== */

#define SHIPMENT_FNV_OFFSET 1469598103934665603ULL
#define SHIPMENT_FNV_PRIME  1099511628211ULL

static void shipment_fold(uint64_t* h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { *h ^= (uint64_t)b[i]; *h *= SHIPMENT_FNV_PRIME; }
}

/* Every file this volume is being given, in the order it is given them: the
 * name as written on the command line, then the bytes. The name is in the
 * number as well as the contents, because two files whose contents were
 * swapped are a different shipment. */
static uint64_t shipment_fingerprint(int file_count, char* argv[], int first) {
    uint64_t h = SHIPMENT_FNV_OFFSET;
    for (int i = 0; i < file_count; i++) {
        const char* path = argv[first + i * 2];
        shipment_fold(&h, path, strlen(path) + 1);
        FILE* f = fopen(path, "rb");
        if (!f) continue;            /* the pack itself will refuse it below */
        static unsigned char buf[1 << 16];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) shipment_fold(&h, buf, got);
        fclose(f);
        shipment_fold(&h, "", 1);
    }
    return h;
}

/* Read the plate out of the kernel being packed. Found by its tag, exactly as
 * tools/buildplate finds it, and required to appear exactly once — the same
 * rule, because a kernel with two plates is a kernel nothing can vouch for. */
static void kernel_plate(const char* path, uint64_t* tree, uint32_t* verdict) {
    *tree = 0;
    *verdict = BUILDPLATE_VERDICT_UNSTAMPED;
    if (!path) return;

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= (long)sizeof(BuildPlate)) { fclose(f); return; }
    unsigned char* img = (unsigned char*)malloc((size_t)n);
    if (!img) { fclose(f); return; }
    if (fread(img, 1, (size_t)n, f) != (size_t)n) { free(img); fclose(f); return; }
    fclose(f);

    static const char tag[BUILDPLATE_TAG_BYTES] = BUILDPLATE_TAG_INIT;
    long at = -1; int seen = 0;
    for (size_t i = 0; i + sizeof(BuildPlate) <= (size_t)n; i++) {
        if (memcmp(img + i, tag, BUILDPLATE_TAG_BYTES) == 0) { seen++; if (at < 0) at = (long)i; }
    }
    if (seen == 1) {
        BuildPlate p;
        memcpy(&p, img + at, sizeof(p));
        if (p.Magic == BUILDPLATE_MAGIC && p.Version == BUILDPLATE_VERSION) {
            *tree = p.TreeFingerprint;
            *verdict = p.Verdict;
        }
    } else if (seen > 1) {
        printf("  WARNING: the kernel being packed carries %d plates; the volume "
               "will say it knows nothing about which tree made it\n", seen);
    }
    free(img);
}

/*
 * The shipment's own half of the pedigree, computed once and written into
 * both copies of the Deed. The kernel's verdict rides along, copied out of the
 * plate in the kernel file itself — so a volume packed over a harness
 * injection says UNDER TEST at mount even if the kernel it carries never gets
 * far enough to say it. `kernel_path` is NULL for a volume with no kernel.
 */
static VolumePacked PackedStamp(int file_count, char* argv[], int first,
                                const char* kernel_path) {
    VolumePacked packed;
    memset(&packed, 0, sizeof(packed));
    packed.packed_unix = (uint64_t)time(NULL);
    packed.shipment    = shipment_fingerprint(file_count, argv, first);
    packed.file_count  = (uint32_t)file_count;
    /* Through locals, because VolumePacked is packed and the address of a
     * packed member is not necessarily aligned — the compiler is right to say
     * so, and this tree builds with no warnings. */
    uint64_t plate_tree = 0;
    uint32_t plate_verdict = BUILDPLATE_VERDICT_UNSTAMPED;
    kernel_plate(kernel_path, &plate_tree, &plate_verdict);
    packed.tree    = plate_tree;
    packed.verdict = plate_verdict;
    printf("  Pedigree:      shipment %016llx over %d file(s), tree %016llx, %s\n",
           (unsigned long long)packed.shipment, file_count,
           (unsigned long long)packed.tree,
           packed.verdict == BUILDPLATE_VERDICT_CLEAN      ? "clean" :
           packed.verdict == BUILDPLATE_VERDICT_UNDER_TEST ? "UNDER TEST" :
                                                            "the kernel carries no plate");
    return packed;
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

/* Whether a file wears the label `kernel` — its name stems to it, or it was
 * given it — which is the file the Deed's BOOT stamp points a loader at. */
static int KernelTagged(const uint16_t* ids, uint16_t count) {
    for (uint16_t t = 0; t < count; t++) {
        if (g_tags[ids[t]].value == NULL && strcmp(g_tags[ids[t]].key, "kernel") == 0)
            return 1;
    }
    return 0;
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

/* ====================================================================
 * The ground, and everything written onto it
 *
 * `g_ground_start` is the only absolute address this tool ever holds, it is
 * read out of the medium's own partition table, and every write below is
 * counted from it. Move the partition and the same bytes land in the right
 * place; hand the tool a disk that is not ours and it writes nothing at all.
 * ==================================================================== */

static uint64_t g_ground_start;    /* first sector of the volume's ground */
static uint64_t g_ground_sectors;  /* how far it runs */

/* One place where a volume-relative sector becomes a place in the file. */
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

/* `data_start_block` is where the data run begins, in the volume's blocks; the
 * block number inside it is what a file's metadata carries, so it means the
 * same thing forever no matter what is laid down in front of it. */
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

/* ====================================================================
 * CRC32 (ISO 3309) — the same sum the kernel's KCrc32 takes, which is also
 * the one GPT is specified in, so one routine checks a partition table, a
 * Deed and a Ledger.
 * ==================================================================== */

static uint32_t tagfs_crc32(const uint8_t* data, uint32_t len) {
    /* The pen's, so the routine that signs a Deed and the routine that checks
     * a partition table are one routine and cannot drift apart. */
    return DeedPenSum(data, len);
}

/* ====================================================================
 * Reading the medium's partition table — the same two tables, checked the
 * same way, as the kernel's core/boardroom/ground.c. This is deliberately a
 * second implementation and not a shared one: the kernel's speaks to a medium
 * through the Boardroom and this one seeks in a file, and the day they
 * disagree about where a volume is, is a day worth finding out about at mkfs
 * time rather than at boot.
 * ==================================================================== */

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

/* cf8ae49a-d26a-4959-9a6f-71c932e0c9eb, in the mixed-endian order GPT stores a
 * type GUID in — byte for byte what ground.c compares against. */
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
        g_ground_sectors = last - first + 1;    /* GPT's last is inclusive */
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

/*
 * Find the run of this image claimed for BoxOS. Nothing is written anywhere
 * else — a tool that formats "the whole file" is a tool that eats a disk
 * somebody handed it by mistake.
 */
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

/* ====================================================================
 * Layout computation — in the volume's own 4096-byte blocks, counted from
 * the start of its ground. Nothing here is a sector number on a medium.
 * ==================================================================== */

typedef struct {
    uint32_t volume_blocks;     /* the whole ground, in blocks           */
    uint32_t deed_block;        /* 0 — the title                         */
    uint32_t ledger_block;      /* two copies, ledger_block and +1       */
    uint32_t disk_book_block;   /* head, backup, then the records        */
    uint32_t disk_book_blocks;
    uint32_t bitmap_block;
    uint32_t bitmap_blocks;
    uint32_t data_block;
    uint32_t data_blocks;
    uint64_t tail_sector;       /* where the far copy of the Deed goes   */
} VolumeShape;

/* One bitmap block is 4096 bytes of bits, and a bit is a data block. */
#define BITMAP_BLOCK_COVERS  ((uint32_t)(TAGFS_BLOCK_SIZE * 8))

/*
 * ‼ THE BITMAP IS LAID DOWN FOR THE GROUND THIS VOLUME MAY ONE DAY TAKE,
 * NOT FOR THE GROUND IT IS BORN ON.
 *
 * A volume takes the ground behind it when it is mounted — write this image to
 * a 64 GB stick and the volume grows into the whole stick instead of leaving
 * 63 GB of it dark. Growing means more data blocks, and more data blocks means
 * more bits. But the bitmap lies IN FRONT of the data run: making it bigger
 * later would push `data_block` along, and a block number in a file's metadata
 * has to mean the same thing for the whole life of the volume. So the room is
 * taken at birth, once, and growth never has to move anything.
 *
 * Sixty-four gibibytes is named because that is the size of the media this
 * system is written to. It costs one bit per 4096-byte block — 32 KiB of
 * bitmap per GiB of ceiling, 2 MiB in all — on an image otherwise 90 MiB.
 * A volume BORN bigger than the ceiling still gets a bitmap that covers it:
 * the number below is a floor under the bitmap, not a cap on the volume.
 */
#define GROWTH_CEILING_BLOCKS \
    ((uint32_t)((64ull * 1024 * 1024 * 1024) / TAGFS_BLOCK_SIZE))
#define GROWTH_CEILING_BITMAP_BLOCKS \
    ((GROWTH_CEILING_BLOCKS + BITMAP_BLOCK_COVERS - 1) / BITMAP_BLOCK_COVERS)

/* 0; -1 when the ground is too small to lay a volume on at all; -2 when it is
 * longer than a format-v1 volume can count.
 *
 * ‼ THE SECOND IS A REFUSAL, AND IT USED TO BE A CUT. A format-v1 volume counts
 * its blocks in 32 bits — LAYOUT, the bitmap, every extent — so it reaches
 * 16 TiB and no further, and a longer ground was cut to the low 32 bits of its
 * block count right here, in silence: the volume was laid out over whatever
 * that remainder came to, and the rest of the ground was never spoken of. */
static int compute_layout(uint64_t ground_sectors, VolumeShape* out) {
    memset(out, 0, sizeof(*out));

    if (ground_sectors / TAGFS_BLOCK_SECTORS > UINT32_MAX) return -2;
    out->volume_blocks    = (uint32_t)(ground_sectors / TAGFS_BLOCK_SECTORS);
    out->deed_block       = 0;
    out->ledger_block     = DEED_BLOCKS;
    out->disk_book_block  = out->ledger_block + VOLUME_LEDGER_COPIES;
    out->disk_book_blocks = 2 + DISK_BOOK_ENTRY_BLOCKS;  /* head, backup, records */
    out->bitmap_block     = out->disk_book_block + out->disk_book_blocks;

    /*
     * The bitmap covers the data run and lies in front of it, so its size and
     * the run's size define each other. It starts at the growth ceiling and
     * only ever GROWS from there.
     *
     * ‼ ONLY EVER GROWS, AND THAT IS WHAT MAKES IT SETTLE.
     *
     * A loop that could also shrink can go round in a two-cycle: N blocks
     * leave a run that needs N+1, and N+1 blocks take a block off the front so
     * the run needs only N again. It then runs out of passes and leaves
     * whichever of the two it was holding — and half the time that is the
     * smaller one, which is a volume whose data run is a block wider than its
     * own bitmap can account for. Growing only cannot cycle: a bigger bitmap
     * makes a shorter run, and a shorter run can only ask for less.
     */
    uint32_t bitmap_blocks = GROWTH_CEILING_BITMAP_BLOCKS;
    for (int i = 0; i < 32; i++) {
        uint32_t data_block = out->bitmap_block + bitmap_blocks;
        /* ‼ Checked, because it can now happen. With a two-megabyte bitmap the
         * front matter is 645 blocks, and a ground smaller than that used to
         * make this subtraction wrap to four billion and be believed. */
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

    /* Said as a fact rather than trusted from the loop above: a volume whose
     * bitmap cannot hold a bit for every block of its data run is a volume
     * that cannot keep track of its own ground, and the kernel refuses to
     * mount one. It must not be possible to MAKE one. */
    if ((uint64_t)out->data_blocks >
        (uint64_t)out->bitmap_blocks * BITMAP_BLOCK_COVERS) return -1;

    /* The far copy goes in the last block of the volume — as far from the head
     * as the ground allows, which is the whole point of having it. */
    out->tail_sector = (uint64_t)(out->volume_blocks - DEED_BLOCKS) * TAGFS_BLOCK_SECTORS;
    return 0;
}

/* ====================================================================
 * Writing the Deed
 *
 * Two copies of one record, at opposite ends of the volume. Both are built
 * here from the same numbers, so the only thing that differs between them is
 * the one byte that says which is which — and a head read where a tail should
 * be is then a misdirected read that says so, instead of a valid-looking Deed.
 * ==================================================================== */

/* The GEOMETRY stamp, the same for every volume this tool makes. */
static VolumeGeometry DeedGeometry(void) {
    VolumeGeometry geo;
    memset(&geo, 0, sizeof(geo));
    geo.logical_bytes  = TAGFS_SECTOR_BYTES;
    /* What the volume was laid out FOR, which is a decision made here and not
     * a fact about the image file: everything below sits on a 4096-byte grid.
     * The kernel compares this against what the medium says of itself, and
     * says so when a volume assumed a finer grid than the medium has. */
    geo.physical_bytes = TAGFS_BLOCK_SIZE;
    geo.grain_bytes    = 1024u * 1024u;      /* the volume starts on a 1 MiB step */
    geo.block_bytes    = TAGFS_BLOCK_SIZE;
    return geo;
}

static int write_deed(FILE* disk, uint32_t role, uint64_t vsector,
                      const uint8_t uuid[16], const VolumeShape* shape,
                      const VolumeBoot* boot, uint32_t mpool_blocks,
                      uint32_t ftable_blocks, uint32_t registry_blocks,
                      const VolumePacked* packed) {
    uint8_t block[TAGFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    VolumeGeometry geo = DeedGeometry();

    VolumeLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.total_blocks         = shape->volume_blocks;
    layout.state_block          = shape->ledger_block;
    layout.state_blocks         = VOLUME_LEDGER_COPIES;
    layout.tag_registry_block   = shape->data_block + 0;
    layout.file_table_block     = shape->data_block + 1;
    layout.metadata_pool_block  = shape->data_block + 2;
    /* ‼ HOW LONG THESE THREE CHAINS ARE IS STATED AND NOT BELIEVED.
     *
     * A Deed is written once and read for the rest of the volume's life, and
     * all three of these chains lengthen with ordinary use. file_table_blocks
     * was a hard-coded 1 here, so a volume that grew past 510 files lost every
     * entry above that on its next mount; metadata_pool_blocks was right the
     * day it was written and short of the truth ever after, and the kernel
     * walking only that far severed the rest of the pool on every boot.
     * tag_registry_blocks was a hard-coded 1 as well and is now the truth at
     * birth, on the same terms as the other two.
     *
     * The kernel walks all three chains to their end and reads none of these
     * fields. They are still written, truthfully, because the layout stamp is
     * shared with anything else that reads a volume — and a number that is
     * right when the volume is born is worth more than a zero. */
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

    /* The pen zeroes the block, lays the magic and the prologue, and knows
     * where each stamp goes. Nothing about the FORMAT is spelled here any
     * more — only which stamps this volume is born with and what they say. */
    DeedPen pen;
    DeedPenOpen(&pen, block, sizeof(block));
    if (DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo,    sizeof(geo))    != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_LAYOUT,   &layout, sizeof(layout)) != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BORN,     &born,   sizeof(born))   != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_BOOT,     boot,    sizeof(*boot))  != 0 ||
        DeedPenSay(&pen, VOLUME_STAMP_PACKED,   packed,  sizeof(*packed)) != 0) {
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

/* ====================================================================
 * Writing the Ledger
 *
 * Both copies, identical, both valid: a volume that has never been written to
 * still has to mount, and a mount reads whichever copy is newer. They carry
 * the same seq here, and the first write the kernel makes goes to the other
 * one with seq+1 — from then on the pair alternates forever.
 *
 * Both are SEALED (volume_ledger.h, SettleMark): mkfs is the volume's first
 * settled cycle, and a machine stopping cleanly — the pool from its head to
 * its last record and the registry to its last record, with their CRC32s — so
 * the first mount stands on a seal like every mount after it.
 * ==================================================================== */

/* A closed pool block's part in the seal's fold (volume_ledger.h,
 * closed_crc32): the CRC32 of its records, summed again with the block's
 * number and the records' length — metadata_pool/meta_pool.c PoolBlockMix,
 * the same twelve bytes, little-endian. */
static uint32_t pool_block_mix(uint32_t block, const MetaPoolBlock* blk) {
    uint32_t mix[3] = { block, blk->used_bytes, tagfs_crc32(blk->payload, blk->used_bytes) };
    return tagfs_crc32((const uint8_t*)mix, sizeof(mix));
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

/* ====================================================================
 * The integrity map
 *
 * One 64-bit digest per block of the data run (tagfs/integrity/), a zero
 * digest meaning "unknown", laid down here already zeroed: a mount never lays
 * a map, so it never writes and flushes a run of zeros the size of the volume
 * before the volume is up (RT-15). The Ledger names it (write_ledger's
 * caller).
 * ==================================================================== */

static int write_integrity_map(FILE* disk, uint32_t data_start_block, uint32_t first,
                               uint32_t count, uint32_t total_blocks) {
    if (first + count > total_blocks) {
        fprintf(stderr, "Not enough blocks for the integrity map (%u at %u)\n", count, first);
        return -1;
    }
    uint8_t zero[TAGFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < count; i++) {
        if (write_block(disk, data_start_block, first + i, zero) != 0) {
            fprintf(stderr, "Failed to write integrity map block %u\n", first + i);
            return -1;
        }
    }
    printf("  Integrity map: %u block(s) at data block %u\n", count, first);
    return 0;
}

/* ====================================================================
 * Build tag registry block
 * ==================================================================== */

/* How many bytes one tag takes in a registry block. One expression, used by
 * the writer below and by the ground estimate, so the two cannot drift. */
static uint32_t registry_record_size(const TagEntry* t) {
    uint32_t key_len   = (uint32_t)strlen(t->key);
    uint32_t value_len = t->value ? (uint32_t)strlen(t->value) : 0;
    return 2 + 1 + 1 + 2 + key_len + value_len;   /* tag_id, flags, key_len, value_len */
}

/*
 * ‼ THE REGISTRY CHAINS, THE SAME WAY THE POOL AND THE TABLE ALREADY DO.
 *
 * It used to be one block, and a vocabulary that outgrew it did not chain — it
 * printed "Tag registry block overflow" and the build stopped. Measured at 416
 * tags. Every tag id on a volume is defined in this record, so that was a hard
 * ceiling on how much a BoxOS image is allowed to be ABOUT, discovered by the
 * build breaking rather than by anything saying so.
 *
 * TagRegistryBlock has carried a next_block field the whole time and the kernel
 * follows it; nothing ever wrote one. The continuation blocks come out of the
 * same high-water mark the pool's and the table's do, so the bitmap written
 * later marks them used.
 */
static int write_registry_chain(FILE* disk, uint32_t data_start_block,
                                uint32_t* next_free, uint32_t total_blocks,
                                uint32_t* out_blocks, SettleMark* mark) {
    TagRegistryBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic = TAGFS_REGISTRY_MAGIC;

    uint32_t here   = 0;   /* data block 0 — where the Deed says the registry begins */
    uint32_t blocks = 1;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_tag_count; i++) {
        uint8_t  key_len   = (uint8_t)strlen(g_tags[i].key);
        uint16_t value_len = g_tags[i].value ? (uint16_t)strlen(g_tags[i].value) : 0;
        uint8_t  flags     = g_tags[i].value ? 1 : 0;

        uint32_t record_size = registry_record_size(&g_tags[i]);

        /* A record is never split across blocks — the kernel's loader walks a
         * block's data[] and stops at its end — so one that cannot fit in an
         * empty block cannot be stored at all, and chaining will not save it. */
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

        /* tag_id (uint16_t) — the kernel RESTORES the tag at this exact id on
         * load (tag_registry.c RegistryAdopt), so the intern order
         * above IS the on-disk ID contract, not a throwaway field. */
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
        blk.entry_count++;
        blk.used_bytes = (uint16_t)offset;
    }

    if (write_block(disk, data_start_block, here, &blk) != 0) {
        fprintf(stderr, "Failed to write tag registry block %u\n", here);
        return -1;
    }

    /* The seal's part of the registry: its last block, and its records. */
    mark->registry_block = here;
    mark->registry_bytes = blk.used_bytes;
    mark->registry_crc32 = tagfs_crc32(blk.data, blk.used_bytes);

    *out_blocks = blocks;
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

/*
 * ‼ A FILE IS LAID IN ONE RUN AND NAMED BY AS MANY EXTENTS AS THAT RUN NEEDS.
 *
 * An extent counts its blocks in 16 bits, and this used to write the whole
 * file as one extent with its block count cast to them: a file of more than
 * 0xFFFF blocks (256 MiB) went onto the volume named by an extent that held
 * the count modulo 65536 — the kernel read a file cut short and the rest of
 * its blocks stood taken and owned by nothing. The run is now cut into extents
 * of at most 0xFFFF blocks, the most one can count. A record longer than the
 * pool holds is answered with its length and not packed; the caller refuses
 * it.
 */
#define EXTENT_MAX_BLOCKS 0xFFFFu

static uint32_t pack_metadata_record(uint8_t* buf, const FileInfo* fi) {
    const char* fname = fi->filename;
    uint16_t name_len = (uint16_t)strlen(fname);
    uint32_t extents  = (fi->block_count + EXTENT_MAX_BLOCKS - 1) / EXTENT_MAX_BLOCKS;

    uint32_t record_size = RECORD_HEADER_SIZE
                         + fi->tag_count * (uint32_t)sizeof(uint16_t)
                         + extents * (uint32_t)sizeof(FileExtent)
                         + name_len;
    if (record_size > TAGFS_MPOOL_DATA_SIZE)
        return record_size;
    uint16_t record_len   = (uint16_t)record_size;
    uint16_t extent_count = (uint16_t)extents;

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

    /* extents[] — the file's one run, cut at the width of an extent */
    for (uint32_t e = 0; e < extents; e++) {
        uint32_t   left = fi->block_count - e * EXTENT_MAX_BLOCKS;
        FileExtent ext;
        ext.start_block = fi->start_block + e * EXTENT_MAX_BLOCKS;
        ext.block_count = (uint16_t)(left < EXTENT_MAX_BLOCKS ? left : EXTENT_MAX_BLOCKS);
        memcpy(buf + pos, &ext, sizeof(FileExtent));
        pos += sizeof(FileExtent);
    }

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

/* ‼ FROM THE SYSTEM'S ENTROPY, NOT FROM THE CLOCK. It was rand() seeded with
 * the second the volume was made, so two volumes made in the same second came
 * out as the same volume — and the identity is what the loader writes on the
 * boarding pass and the kernel picks its volume by, out of every medium in the
 * machine. A tool that cannot read /dev/urandom refuses rather than invent
 * one. 0, or -1 having said why. */
static int generate_uuid(uint8_t uuid[16]) {
    FILE*  r   = fopen("/dev/urandom", "rb");
    size_t got = r ? fread(uuid, 1, 16, r) : 0;
    if (r) fclose(r);
    if (got != 16) {
        fprintf(stderr, "Cannot read sixteen bytes from /dev/urandom — a volume's "
                        "identity is not made up out of the time of day\n");
        return -1;
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    return 0;
}

/* ====================================================================
 * Main
 * ==================================================================== */

/* ====================================================================
 *  --ground: how far the volume's ground must run to hold these files
 *
 * ‼ THE SIZE OF A VOLUME IS NOT A NUMBER ANYBODY TYPES.
 *
 * It was: the build made a 24 MiB region because 51200 was written in the
 * Makefile, and the day the files inside came to more than that, the build did
 * not say "the medium is too small" — it made the volume anyway, with four
 * hundred kilobytes left, and the C++ suite failed seventy checks because it
 * had nowhere to write its test files. A ceiling that is discovered by what
 * breaks under it is not a ceiling, it is a trap.
 *
 * So the tool that lays a volume out is asked first how much ground it needs,
 * and the answer is what the medium is then made to give. Adding a program to
 * the image grows the image. Nothing has to be raised by hand, ever.
 *
 * The answer is the CONTENT plus the ROOM A RUNNING MACHINE WORKS IN — logs
 * poured by logsave, drafts, CoW snapshots, the files a test suite makes and
 * deletes. That room is a policy, stated once, here, with its reason: a
 * machine that cannot write is a machine that cannot be used, and the cost of
 * being generous is sixty-four mebibytes on a medium that has gigabytes.
 * ==================================================================== */

/* Sixty-four mebibytes of room, in the volume's own blocks. */
#define GROUND_WORKING_ROOM_BLOCKS   ((64u * 1024u * 1024u) / TAGFS_BLOCK_SIZE)

/* The metadata pool chains into the data run as records outgrow one block. The
 * image's own record count settles in two blocks today; eight is the room for
 * that to double twice without anybody noticing it had a bound. */
#define GROUND_METADATA_SLACK_BLOCKS 8u

/* Every volume alignment is on the conventional mebibyte — the grain the Deed
 * records and every flash translation layer is built around. */
#define GROUND_GRAIN_SECTORS         2048u

static int say_the_ground(int pair_count, char* argv[], int first) {
    uint64_t data_blocks = 3;   /* registry, file table, metadata pool */
    data_blocks += GROUND_METADATA_SLACK_BLOCKS;

    /* The file table chains too, now that it is allowed to be longer than 510
     * entries. File ids run 1..pair_count, so the table is pair_count+1 entries
     * and everything past the first block is ground this has to ask for. */
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

    /* And the registry, now that it chains as well. It is asked for by
     * interning the very tags the build is about to write, through the same
     * routines that write them — a second estimate of how big a vocabulary is
     * would be a second thing to keep true. A record is never split, so a
     * block is closed as soon as the next record will not fit, and that is
     * counted here exactly as write_registry_chain counts it. */
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
        data_blocks += reg_blocks - 1;   /* block 0 is already in the 3 above */
    }

    data_blocks += GROUND_WORKING_ROOM_BLOCKS;

    /* And the integrity map mkfs lays down zeroed (write_integrity_map): one
     * digest per block of the run, a block of digests past the estimate for
     * the run the grain rounds up to. */
    data_blocks += (data_blocks + INTEGRITY_DIGESTS_PER_BLOCK - 1) / INTEGRITY_DIGESTS_PER_BLOCK + 1;

    /* compute_layout answers the other way round — ground in, data run out —
     * and the bitmap in between makes it not quite invertible. So the ground
     * is grown a grain at a time until the run it yields is big enough. It
     * settles in one or two steps from this start and cannot loop: each step
     * adds ground, and the run grows with it. */
    uint64_t ground = (data_blocks + 64) * TAGFS_BLOCK_SECTORS;
    ground = ((ground + GROUND_GRAIN_SECTORS - 1) / GROUND_GRAIN_SECTORS) * GROUND_GRAIN_SECTORS;

    for (;;) {
        VolumeShape shape;
        int laid = compute_layout(ground, &shape);
        if (laid == 0 && shape.data_blocks >= data_blocks)
            break;
        if (laid == -2) {
            fprintf(stderr, "create_tagfs --ground: these files need more than the "
                            "16 TiB a format-v1 volume can count\n");
            return 1;
        }
        ground += GROUND_GRAIN_SECTORS;
    }

    printf("%llu\n", (unsigned long long)ground);
    return 0;
}

/* The image opened for writing and the ground somebody claimed on it found —
 * or NULL, having said why. Both kinds of volume are made on exactly this. */
static FILE* GroundOpen(const char* disk_path) {
    FILE* disk = fopen(disk_path, "r+b");
    if (!disk) { perror("Failed to open disk image"); return NULL; }

    fseek(disk, 0, SEEK_END);
    long disk_size = ftell(disk);

    printf("[create_tagfs] Making a BoxOS volume on %s (%lu bytes)\n",
           disk_path, disk_size);

    if (survey_ground(disk) != 0) { fclose(disk); return NULL; }

    if ((long)((g_ground_start + g_ground_sectors) * TAGFS_SECTOR_BYTES) > disk_size) {
        fprintf(stderr, "The BoxOS partition runs to sector %llu and the image "
                        "ends at %llu — it was copied short\n",
                (unsigned long long)(g_ground_start + g_ground_sectors),
                (unsigned long long)(disk_size / TAGFS_SECTOR_BYTES));
        fclose(disk);
        return NULL;
    }
    return disk;
}

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

static int FrameOnlyMake(const char* disk_path, int file_count, char* argv[], int first) {
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
    printf("\n[create_tagfs] The volume is made: format v2, its book not yet written.\n");
    rc = 0;
out:
    for (int i = 0; i < file_count; i++) free(files[i].tag_ids);
    free(files);
    fclose(disk);
    return rc;
}

int main(int argc, char* argv[]) {
    /* --ground answers a question about a file list and touches no medium. */
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        if ((argc - 2) % 2 != 0) {
            fprintf(stderr, "Usage: %s --ground [<file> <tags>] ...\n", argv[0]);
            return 1;
        }
        return say_the_ground((argc - 2) / 2, argv, 2);
    }

    /* --frame-only lays a format-v2 Deed over files and writes no book. */
    if (argc >= 2 && strcmp(argv[1], "--frame-only") == 0) {
        if (argc < 3 || (argc - 3) % 2 != 0) {
            fprintf(stderr, "Usage: %s --frame-only <disk_image> [<file> <tags>] ...\n", argv[0]);
            return 1;
        }
        return FrameOnlyMake(argv[2], (argc - 3) / 2, argv, 3);
    }

    if (argc < 2 || (argc > 2 && (argc - 2) % 2 != 0)) {
        fprintf(stderr, "Usage: %s <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "       %s --ground [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "       %s --frame-only <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
        fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
        fprintf(stderr, "type GUID in a GPT). Nothing outside that run is touched.\n");
        fprintf(stderr, "The file whose name stems to 'kernel' is the one the Deed\n");
        fprintf(stderr, "points a loader at.\n");
        return 1;
    }

    int file_count = (argc - 2) / 2;
    FILE* disk = GroundOpen(argv[1]);
    if (!disk) return 1;

    /* ---- Compute layout, in the volume's own blocks ---- */
    VolumeShape shape;
    int laid = compute_layout(g_ground_sectors, &shape);
    if (laid == -2) {
        fprintf(stderr,
                "The ground claimed for BoxOS is %llu sectors — more than the "
                "16 TiB a format-v1 volume counts its blocks in. It is refused "
                "rather than laid out over a part of it.\n",
                (unsigned long long)g_ground_sectors);
        fclose(disk);
        return 1;
    }
    if (laid != 0) {
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

    /* ---- Parse files and tags ---- */
    FileInfo* files = NULL;
    if (file_count > 0) {
        files = calloc(file_count, sizeof(FileInfo));
        if (!files) { fprintf(stderr, "calloc failed\n"); fclose(disk); return 1; }
    }

    /* Claim ids 0..11 for the reserved vocabulary BEFORE any file tag is
     * interned, so the kernel's privilege masks are representable on mount. */
    seed_reserved_tags();

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

        if (KernelTagged(files[i].tag_ids, files[i].tag_count)) kernel_file_index = i;

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

    /* ---- Write tag registry (block 0, chains if needed) ---- */
    printf("\n[create_tagfs] Writing tag registry...\n");
    uint32_t   reg_blocks = 1;
    SettleMark seal;
    memset(&seal, 0, sizeof(seal));
    if (write_registry_chain(disk, data_start_block, &next_block,
                             total_blocks, &reg_blocks, &seal) != 0) {
        fclose(disk); return 1;
    }
    if (reg_blocks > 1) {
        printf("  Tag registry: %u tags over %u blocks\n", g_tag_count, reg_blocks);
    }

    /* ---- Write file data (blocks 3+) ---- */
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

    /* ---- Write metadata pool (block 2, chains if needed) ---- */
    printf("[create_tagfs] Writing metadata pool...\n");
    MetaPoolBlock mpool;
    memset(&mpool, 0, sizeof(MetaPoolBlock));
    mpool.magic = TAGFS_MPOOL_MAGIC;

    uint32_t current_mpool_block = 2;  /* first mpool block */
    uint32_t mpool_block_count   = 1;  /* how many mpool blocks total */

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

        /* If current block is full, chain to a new block */
        if (mpool.used_bytes + record_size > TAGFS_MPOOL_DATA_SIZE) {
            uint32_t new_mpool_block = next_block++;
            if (new_mpool_block >= total_blocks) {
                fprintf(stderr, "Not enough blocks for metadata pool chain\n");
                fclose(disk); return 1;
            }

            /* Link old block → new block, write old block to disk */
            mpool.next_block = new_mpool_block;
            if (write_block(disk, data_start_block, current_mpool_block, &mpool) != 0) {
                fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
                free(ftable_all); fclose(disk); return 1;
            }
            printf("  Metadata pool: block %u full (%u records, %u bytes), chaining to block %u\n",
                   current_mpool_block, mpool.record_count, mpool.used_bytes, new_mpool_block);
            seal.closed_crc32 ^= pool_block_mix(current_mpool_block, &mpool);

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
        mpool.kept++;

        /* File table: entry at index file_id */
        uint32_t fid = files[i].file_id;
        if (fid < ftable_entries) {
            ftable_all[fid].meta_block  = current_mpool_block;
            ftable_all[fid].meta_offset = meta_offset;
        }
    }

    /* Write final (or only) mpool block */
    if (write_block(disk, data_start_block, current_mpool_block, &mpool) != 0) {
        fprintf(stderr, "Failed to write metadata pool block %u\n", current_mpool_block);
        free(ftable_all); fclose(disk); return 1;
    }
    seal.chain_first     = 2;
    seal.pool_from_block = 2;
    seal.pool_from_bytes = 0;
    seal.pool_to_block   = current_mpool_block;
    seal.pool_to_bytes   = mpool.used_bytes;
    seal.tail_crc32      = tagfs_crc32(mpool.payload, mpool.used_bytes);
    if (mpool_block_count > 1) {
        printf("  Metadata pool: %u blocks total\n", mpool_block_count);
    }

    /* ---- Write file table (block 1, chains if needed) ---- */
    printf("[create_tagfs] Writing file table...\n");
    uint32_t ftable_blocks =
        (ftable_entries + TAGFS_FTABLE_PER_BLOCK - 1) / TAGFS_FTABLE_PER_BLOCK;
    if (ftable_blocks == 0) ftable_blocks = 1;

    {
        uint32_t here = 1;   /* the first file table block, where the Deed says */
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

    /* ---- The integrity map, laid zeroed where the Ledger names it ---- */
    printf("[create_tagfs] Writing the integrity map...\n");
    uint32_t map_first = next_block;
    uint32_t map_count = (total_blocks + INTEGRITY_DIGESTS_PER_BLOCK - 1) / INTEGRITY_DIGESTS_PER_BLOCK;
    if (write_integrity_map(disk, data_start_block, map_first, map_count, total_blocks) != 0) {
        fclose(disk); return 1;
    }
    next_block += map_count;

    /* ---- Write block bitmap ---- */
    printf("[create_tagfs] Writing block bitmap...\n");
    uint32_t bitmap_buf_size = shape.bitmap_blocks * TAGFS_BLOCK_SIZE;
    uint8_t* bitmap = calloc(1, bitmap_buf_size);
    if (!bitmap) { fprintf(stderr, "calloc failed for bitmap\n"); fclose(disk); return 1; }

    /* Mark all used blocks: 0=registry, 1=ftable, 2=mpool, plus any chained
       mpool and file table blocks and all file data blocks. next_block tracks
       the high-water mark. */
    for (uint32_t b = 0; b < next_block && b < total_blocks; b++) {
        bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
    }

    if (write_at_sector(disk, (uint64_t)shape.bitmap_block * TAGFS_BLOCK_SECTORS,
                        bitmap, bitmap_buf_size) != 0) {
        fprintf(stderr, "Failed to write block bitmap\n");
        free(bitmap); fclose(disk); return 1;
    }
    free(bitmap);

    /* ---- Write the DiskBook's head, its backup, and an empty record run ---- */
    printf("[create_tagfs] Writing DiskBook...\n");
    {
        /* Must match the kernel's DiskBookSuperblock (disk_book.h):
         *   u32 magic, u32 version, u64 start_sector, u32 capacity,
         *   u32 count, u32 generation, u32 flags, u32 crc32, u8 uuid[16], ...
         * `start_sector` is counted from the start of the volume, like every
         * other number the volume keeps about itself. */
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
        /* crc32 at offset 32 (after generation@24, flags@28), over the 512-byte
         * block with the crc field zeroed — matches the kernel's DiskBook SB CRC
         * so a freshly-imaged volume validates without a first-mount reformat. */
        uint32_t dcrc = tagfs_crc32(dbuf, sizeof(dbuf));
        memcpy(dbuf + 32, &dcrc, 4);

        /* A block apart, not a sector: the head and the copy of the head were
         * inside one physical block, so the copy died with what it was for. */
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

    /* ---- The Ledger: what is true of this volume the moment it is made ---- */
    printf("[create_tagfs] Writing the Ledger...\n");

    uint32_t used_blocks = next_block;
    uint32_t free_blocks = total_blocks - used_blocks;

    uint64_t made_at = (uint64_t)time(NULL);

    uint8_t volume_uuid[16];
    if (generate_uuid(volume_uuid) != 0) { fclose(disk); return 1; }

    VolumeLedger led;
    memset(&led, 0, sizeof(led));
    led.seq          = 1;
    led.written_unix = made_at;
    led.free_blocks  = free_blocks;
    led.total_files  = (uint64_t)file_count;
    led.next_file_id = (uint32_t)(file_count + 1);
    led.next_tag_id  = g_tag_count;
    led.total_tags   = g_tag_count;
    led.integrity_map_block  = map_first;
    led.integrity_map_blocks = map_count;
    led.mark         = seal;
    led.mark.settle  = led.seq;
    led.mark.flags   = SETTLE_CLEAN;

    if (write_ledger(disk, &shape, &led) != 0) {
        fprintf(stderr, "Failed to write the Ledger\n");
        fclose(disk); return 1;
    }

    /* ---- The Deed: at the head of the ground, and again at the far end ---- */
    printf("[create_tagfs] Writing the Deed...\n");

    VolumeBoot boot;
    memset(&boot, 0, sizeof(boot));
    if (kernel_file_index >= 0) {
        FileInfo* kf = &files[kernel_file_index];
        /* In the volume's own blocks, not the data run's — so a loader that has
         * the Deed and nothing else can turn it into a sector without first
         * understanding where the data run begins. */
        boot.kernel_block  = shape.data_block + kf->start_block;
        boot.kernel_blocks = kf->block_count;
        boot.kernel_bytes  = (uint32_t)kf->file_size;
        printf("  Boot:          kernel at volume block %u (%u blocks, %u bytes)\n",
               boot.kernel_block, boot.kernel_blocks, boot.kernel_bytes);
    } else {
        printf("  WARNING: nothing here is tagged 'kernel' — this volume carries "
               "no kernel and no loader will boot from it\n");
    }

    VolumePacked packed = PackedStamp(file_count, argv, 2,
                                      kernel_file_index >= 0 ? files[kernel_file_index].filepath : NULL);

    if (write_deed(disk, VOLUME_DEED_ROLE_HEAD, 0,
                   volume_uuid, &shape, &boot, mpool_block_count,
                   ftable_blocks, reg_blocks, &packed) != 0) {
        fprintf(stderr, "Failed to write the Deed\n");
        fclose(disk); return 1;
    }
    if (write_deed(disk, VOLUME_DEED_ROLE_TAIL, shape.tail_sector,
                   volume_uuid, &shape, &boot, mpool_block_count,
                   ftable_blocks, reg_blocks, &packed) != 0) {
        fprintf(stderr, "Failed to write the far copy of the Deed\n");
        fclose(disk); return 1;
    }

    fclose(disk);

    printf("\n[create_tagfs] The volume is made.\n");
    printf("  Files:  %d\n", file_count);
    printf("  Tags:   %u\n", g_tag_count);
    printf("  Blocks: %u used / %u data (%u free)\n", used_blocks, total_blocks, free_blocks);

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
