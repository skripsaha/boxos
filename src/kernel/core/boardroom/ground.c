#include "ground.h"
#include "boardroom.h"
#include "klib.h"
#include "crypto.h"

/*
 * The type that marks a run of a medium as ours.
 *
 * MBR has one byte for it and BoxOS has used 0x7F since the image was first
 * laid out. GPT has sixteen, and this is the value minted for it — permanent,
 * never reissued, and written in the mixed-endian order GPT stores a GUID in
 * (first three fields little-endian), which is the order it appears on the
 * medium and therefore the order to compare in:
 *
 *      cf8ae49a-d26a-4959-9a6f-71c932e0c9eb
 */
#define GROUND_MBR_TYPE  0x7Fu

static const uint8_t g_boxos_type_guid[16] = {
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb
};

/* The one MBR type that does not mean a partition: it means "this disk is
 * really a GPT, and this entry is here so a tool that only knows MBR sees the
 * whole disk as taken instead of as empty". UEFI 2.10 §5.2.3. */
#define GROUND_MBR_TYPE_PROTECTIVE  0xEEu

/* ── MBR ─────────────────────────────────────────────────────────────────
 * Sector 0. Four entries of sixteen bytes at offset 446, then 0x55 0xAA. An
 * entry is: status, three bytes of CHS nobody has used in thirty years, the
 * type byte, three more CHS, then the two numbers that matter — first LBA and
 * sector count, both little-endian 32-bit.
 * ──────────────────────────────────────────────────────────────────────── */
#define MBR_TABLE_OFFSET     446u
#define MBR_ENTRY_BYTES      16u
#define MBR_ENTRY_COUNT      4u
#define MBR_ENTRY_TYPE       4u
#define MBR_ENTRY_START_LBA  8u
#define MBR_ENTRY_SECTORS    12u

/* ── GPT ─────────────────────────────────────────────────────────────────
 * UEFI 2.10 §5.3. The header is at LBA 1 and states its own size, which is
 * what the header checksum covers — not a fixed 92, because a later revision
 * is allowed to be longer and a reader that assumes the length gets the
 * checksum wrong on hardware it has never seen.
 * ──────────────────────────────────────────────────────────────────────── */
#define GPT_HEADER_LBA          1u
#define GPT_SIG_OFFSET          0u
#define GPT_HEADER_SIZE_OFFSET  0x0Cu
#define GPT_HEADER_CRC_OFFSET   0x10u
#define GPT_ENTRY_LBA_OFFSET    0x48u
#define GPT_ENTRY_COUNT_OFFSET  0x50u
#define GPT_ENTRY_BYTES_OFFSET  0x54u
#define GPT_ENTRY_CRC_OFFSET    0x58u

#define GPT_ENTRY_TYPE_OFFSET   0u
#define GPT_ENTRY_FIRST_OFFSET  0x20u
#define GPT_ENTRY_LAST_OFFSET   0x28u

/* A header shorter than the fields it must contain is not a header, and one
 * longer than a sector is not one this reads. */
#define GPT_HEADER_MIN_BYTES    92u

/* The spec reserves at least 16 KiB for the entry array and every tool in use
 * writes 128 entries of 128 bytes. This is the ceiling on what will be read,
 * so a header stating an absurd array cannot be talked into a huge allocation
 * or an overflowing multiply. */
#define GPT_ARRAY_MAX_BYTES     (128u * 1024u)
#define GPT_ENTRY_MIN_BYTES     128u

static const uint8_t g_gpt_signature[8] = { 'E','F','I',' ','P','A','R','T' };

const char *GroundOriginName(GroundOrigin origin)
{
    switch (origin) {
    case GROUND_FROM_MBR: return "MBR";
    case GROUND_FROM_GPT: return "GPT";
    default:              return "?";
    }
}

/* Little-endian readers. The tables are little-endian by specification and
 * this kernel runs little-endian, but reading them byte by byte says so and
 * costs nothing at these sizes — and it sidesteps the unaligned struct access
 * that reading a 128-byte GPT entry as a C type invites. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static bool ground_room(uint8_t found, uint8_t max, uint8_t seat)
{
    if (found < max) return true;
    kprintf("[Ground] seat %u carries more BoxOS ground than this survey "
            "holds (%u); the rest was not looked at\n", seat, max);
    return false;
}

/*
 * GPT, if the disk has one.
 *
 * Both checksums are verified because both exist for a reason: a header that
 * passes its own CRC can still point at an entry array that was interrupted
 * mid-write, and mounting out of that array is mounting out of whatever was
 * there before. A table that does not check out yields nothing and says which
 * check failed — the alternative is a machine that mounts something it cannot
 * justify.
 */
static uint8_t ground_survey_gpt(uint8_t seat, MediumGround *out, uint8_t max)
{
    uint8_t header[BOARDROOM_SECTOR_BYTES];
    if (BoardroomRead(seat, GPT_HEADER_LBA, 1, header) != 0) {
        kprintf("[Ground] seat %u: the disk says it is a GPT and then would "
                "not give up its header\n", seat);
        return 0;
    }

    if (memcmp(header + GPT_SIG_OFFSET, g_gpt_signature, 8) != 0) {
        kprintf("[Ground] seat %u: a protective MBR with no GPT behind it\n",
                seat);
        return 0;
    }

    uint32_t header_bytes = le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES ||
        header_bytes > BOARDROOM_SECTOR_BYTES) {
        kprintf("[Ground] seat %u: its GPT header states a length of %u, "
                "which cannot be one\n", seat, header_bytes);
        return 0;
    }

    /* The header's own checksum is taken over `header_bytes` with the checksum
     * field itself zeroed (UEFI 2.10 §5.3.2). Copied rather than patched in
     * place so the buffer still holds what the medium holds. */
    uint8_t  probe[BOARDROOM_SECTOR_BYTES];
    memcpy(probe, header, header_bytes);
    memset(probe + GPT_HEADER_CRC_OFFSET, 0, 4);
    uint32_t want_header_crc = le32(header + GPT_HEADER_CRC_OFFSET);
    uint32_t have_header_crc = KCrc32(probe, header_bytes);
    if (have_header_crc != want_header_crc) {
        kprintf("[Ground] seat %u: its GPT header does not match its own "
                "checksum (0x%08x against 0x%08x) — not reading it\n",
                seat, have_header_crc, want_header_crc);
        return 0;
    }

    uint64_t entry_lba   = le64(header + GPT_ENTRY_LBA_OFFSET);
    uint32_t entry_count = le32(header + GPT_ENTRY_COUNT_OFFSET);
    uint32_t entry_bytes = le32(header + GPT_ENTRY_BYTES_OFFSET);
    uint32_t want_array_crc = le32(header + GPT_ENTRY_CRC_OFFSET);

    if (entry_bytes < GPT_ENTRY_MIN_BYTES || entry_count == 0 ||
        entry_bytes > GPT_ARRAY_MAX_BYTES ||
        entry_count > GPT_ARRAY_MAX_BYTES / entry_bytes) {
        kprintf("[Ground] seat %u: its GPT states %u entries of %u bytes, "
                "which is not a table this reads\n",
                seat, entry_count, entry_bytes);
        return 0;
    }

    uint32_t array_bytes = entry_count * entry_bytes;
    uint32_t array_secs  = (array_bytes + BOARDROOM_SECTOR_BYTES - 1) /
                           BOARDROOM_SECTOR_BYTES;

    uint8_t *array = (uint8_t *)kmalloc(array_secs * BOARDROOM_SECTOR_BYTES);
    if (!array) {
        kprintf("[Ground] seat %u: no memory to read its GPT entries\n", seat);
        return 0;
    }

    uint8_t found = 0;
    if (BoardroomRead(seat, entry_lba, array_secs, array) != 0) {
        kprintf("[Ground] seat %u: would not give up its GPT entries\n", seat);
        goto done;
    }

    /* The array checksum covers exactly count × size bytes, not the sectors
     * they were read in. */
    uint32_t have_array_crc = KCrc32(array, array_bytes);
    if (have_array_crc != want_array_crc) {
        kprintf("[Ground] seat %u: its GPT entries do not match their own "
                "checksum (0x%08x against 0x%08x) — not reading them\n",
                seat, have_array_crc, want_array_crc);
        goto done;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = array + (uint64_t)i * entry_bytes;
        if (memcmp(e + GPT_ENTRY_TYPE_OFFSET, g_boxos_type_guid, 16) != 0) {
            continue;
        }

        uint64_t first = le64(e + GPT_ENTRY_FIRST_OFFSET);
        uint64_t last  = le64(e + GPT_ENTRY_LAST_OFFSET);
        if (last < first) {
            kprintf("[Ground] seat %u: GPT entry %u ends before it begins\n",
                    seat, i);
            continue;
        }

        if (!ground_room(found, max, seat)) break;

        out[found].start_sector = first;
        out[found].sectors      = last - first + 1;   /* GPT's last is inclusive */
        out[found].origin       = GROUND_FROM_GPT;
        out[found].entry        = (uint8_t)i;
        found++;
    }

done:
    kfree(array);
    return found;
}

uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max)
{
    if (!out || max == 0) return 0;

    uint8_t sector0[BOARDROOM_SECTOR_BYTES];
    if (BoardroomRead(seat, 0, 1, sector0) != 0) {
        kprintf("[Ground] seat %u: could not be read at all\n", seat);
        return 0;
    }

    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        /* No table. Not an error and not ours to fix: a medium may hold
         * anything. It is said because "this disk has no partition table" and
         * "this disk has no BoxOS partition" are different answers to the same
         * question and only one of them is worth acting on. */
        kprintf("[Ground] seat %u: no partition table on it\n", seat);
        return 0;
    }

    /* A protective entry anywhere in the four means the real table is a GPT.
     * Checked before the MBR entries are believed, because a GPT disk's MBR
     * describes a disk that does not exist. */
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        if (e[MBR_ENTRY_TYPE] == GROUND_MBR_TYPE_PROTECTIVE) {
            return ground_survey_gpt(seat, out, max);
        }
    }

    uint8_t found = 0;
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t *e = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        if (e[MBR_ENTRY_TYPE] != GROUND_MBR_TYPE) continue;

        uint64_t start = le32(e + MBR_ENTRY_START_LBA);
        uint64_t count = le32(e + MBR_ENTRY_SECTORS);
        if (count == 0) {
            kprintf("[Ground] seat %u: MBR entry %u claims no sectors\n",
                    seat, i);
            continue;
        }

        if (!ground_room(found, max, seat)) break;

        out[found].start_sector = start;
        out[found].sectors      = count;
        out[found].origin       = GROUND_FROM_MBR;
        out[found].entry        = (uint8_t)i;
        found++;
    }

    return found;
}
