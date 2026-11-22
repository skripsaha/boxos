/*
 * create_tagfs_ground.c — the ground a volume is made on, and every write onto it.
 *
 * The partition table read the way the kernel reads it, the one place a
 * volume-relative sector becomes a place in the image file, the CRC32 that
 * signs a Deed and checks a GPT, the volume's identity, and the GEOMETRY stamp
 * every volume this tool makes is born with — whichever format it is.
 */

#include "create_tagfs.h"

/* What a BoxOS partition looks like from the outside. The MBR type byte and
 * the GPT type GUID are the same two constants the kernel's ground.c reads;
 * they are permanent and are never reissued. */
#define GROUND_MBR_TYPE_BOXOS      0x7F
#define GROUND_MBR_TYPE_PROTECTIVE 0xEE

/* ====================================================================
 * The ground, and everything written onto it
 *
 * `g_ground_start` is the only absolute address this tool ever holds, it is
 * read out of the medium's own partition table, and every write below is
 * counted from it. Move the partition and the same bytes land in the right
 * place; hand the tool a disk that is not ours and it writes nothing at all.
 * ==================================================================== */

uint32_t g_medium_block = TAGFS_SECTOR_BYTES;   /* every number of its partition table counts in it */
uint64_t g_ground_start;    /* first sector of the volume's ground */
uint64_t g_ground_sectors;  /* how far it runs */

/* One place where a volume-relative sector becomes a place in the file. */
int write_at_sector(FILE* disk, uint64_t vsector, const void* data, size_t size) {
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

int write_file_data(FILE* disk, uint32_t data_start_block, uint32_t block,
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

uint32_t tagfs_crc32(const uint8_t* data, uint32_t len) {
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
    const uint64_t per = g_medium_block / TAGFS_SECTOR_BYTES;
    uint8_t header[TAGFS_BLOCK_SIZE];
    if (read_at_sector(disk, GPT_HEADER_LBA * per, header, g_medium_block) != 0) {
        fprintf(stderr, "This image says it is a GPT and has no header behind it\n");
        return -1;
    }
    if (memcmp(header, "EFI PART", 8) != 0) {
        fprintf(stderr, "A protective MBR with no GPT behind it\n");
        return -1;
    }

    uint32_t header_bytes = le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES || header_bytes > g_medium_block) {
        fprintf(stderr, "Its GPT header states a length of %u, which cannot be one\n",
                header_bytes);
        return -1;
    }

    uint8_t probe[TAGFS_BLOCK_SIZE];
    memcpy(probe, header, header_bytes);
    memset(probe + GPT_HEADER_CRC_OFFSET, 0, 4);
    if (tagfs_crc32(probe, header_bytes) != le32(header + GPT_HEADER_CRC_OFFSET)) {
        fprintf(stderr, "Its GPT header does not match its own checksum\n");
        return -1;
    }

    uint64_t entry_lba   = le64(header + GPT_ENTRY_LBA_OFFSET) * per;
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

        g_ground_start   = first * per;
        g_ground_sectors = (last - first + 1) * per;    /* GPT's last is inclusive */
        printf("  Ground:        sectors %llu..%llu, from GPT entry %u\n",
               (unsigned long long)g_ground_start,
               (unsigned long long)(g_ground_start + g_ground_sectors - 1), i);
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

        uint64_t start = le32(e + MBR_ENTRY_START_LBA) * (g_medium_block / TAGFS_SECTOR_BYTES);
        uint64_t count = le32(e + MBR_ENTRY_SECTORS) * (g_medium_block / TAGFS_SECTOR_BYTES);
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
 * UUID generation
 * ==================================================================== */

/* ‼ FROM THE SYSTEM'S ENTROPY, NOT FROM THE CLOCK. It was rand() seeded with
 * the second the volume was made, so two volumes made in the same second came
 * out as the same volume — and the identity is what the loader writes on the
 * boarding pass and the kernel picks its volume by, out of every medium in the
 * machine. A tool that cannot read /dev/urandom refuses rather than invent
 * one. 0, or -1 having said why. */
int generate_uuid(uint8_t uuid[16]) {
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

/* The image opened for writing and the ground somebody claimed on it found —
 * or NULL, having said why. Both kinds of volume are made on exactly this. */
FILE* GroundOpen(const char* disk_path) {
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

/* The GEOMETRY stamp, the same for every volume this tool makes. */
VolumeGeometry DeedGeometry(void) {
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
