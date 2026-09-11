#include "ground.h"
#include "boardroom.h"
#include "klib.h"
#include "crypto.h"

#define GROUND_MBR_TYPE  0x7Fu

static const uint8_t g_boxos_type_guid[16] = {
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb
};

#define GROUND_MBR_TYPE_PROTECTIVE  0xEEu

#define MBR_TABLE_OFFSET     446u
#define MBR_ENTRY_BYTES      16u
#define MBR_ENTRY_COUNT      4u
#define MBR_ENTRY_TYPE       4u
#define MBR_ENTRY_START_LBA  8u
#define MBR_ENTRY_SECTORS    12u

#define GPT_HEADER_LBA          1u
#define GPT_SIG_OFFSET          0u
#define GPT_HEADER_SIZE_OFFSET  0x0Cu
#define GPT_HEADER_CRC_OFFSET   0x10u
#define GPT_MY_LBA_OFFSET       0x18u
#define GPT_ALT_LBA_OFFSET      0x20u
#define GPT_ENTRY_LBA_OFFSET    0x48u
#define GPT_ENTRY_COUNT_OFFSET  0x50u
#define GPT_ENTRY_BYTES_OFFSET  0x54u
#define GPT_ENTRY_CRC_OFFSET    0x58u

#define GPT_ENTRY_TYPE_OFFSET   0u
#define GPT_ENTRY_FIRST_OFFSET  0x20u
#define GPT_ENTRY_LAST_OFFSET   0x28u

#define GPT_HEADER_MIN_BYTES    92u

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

typedef struct GroundMemo {
    struct GroundMemo *next;
    uint8_t      seat;
    uint32_t     seating;
    uint8_t      claimed;
    MediumGround ground[GROUND_MAX_PER_MEDIUM];
} GroundMemo;

static GroundMemo *g_memos = NULL;
static spinlock_t  g_memo_lock;
static bool        g_memo_lock_ready = false;

static void memo_lock_init(void)
{
    if (!g_memo_lock_ready) {
        spinlock_init(&g_memo_lock);
        g_memo_lock_ready = true;
    }
}

static bool memo_recall(uint8_t seat, uint32_t seating, MediumGround *out,
                        uint8_t *out_claimed)
{
    memo_lock_init();

    spin_lock(&g_memo_lock);
    for (GroundMemo *m = g_memos; m; m = m->next) {
        if (m->seat == seat && m->seating == seating) {
            *out_claimed = m->claimed;
            memcpy(out, m->ground, (size_t)m->claimed * sizeof(MediumGround));
            spin_unlock(&g_memo_lock);
            return true;
        }
    }
    spin_unlock(&g_memo_lock);
    return false;
}

static void memo_keep(uint8_t seat, uint32_t seating,
                      const MediumGround *ground, uint8_t claimed)
{
    memo_lock_init();

    GroundMemo *spare = (GroundMemo *)kmalloc(sizeof(GroundMemo));

    spin_lock(&g_memo_lock);

    GroundMemo *m = NULL;
    for (GroundMemo *c = g_memos; c; c = c->next) {
        if (c->seat == seat) {
            m = c;
            break;
        }
    }

    if (!m) {
        if (!spare) {
            spin_unlock(&g_memo_lock);
            return;
        }
        m       = spare;
        spare   = NULL;
        m->seat = seat;
        m->next = g_memos;
        g_memos = m;
    }

    m->seating = seating;
    m->claimed = claimed;
    memcpy(m->ground, ground, (size_t)claimed * sizeof(MediumGround));

    spin_unlock(&g_memo_lock);

    if (spare) kfree(spare);
}

typedef enum {
    GPT_TABLE_READ,
    GPT_TABLE_ABSENT,
    GPT_TABLE_DAMAGED
} GptVerdict;

static GptVerdict gpt_read_one(uint8_t seat, uint64_t header_lba,
                               uint64_t medium_sectors,
                               MediumGround *out, uint8_t max, uint8_t *found)
{
    *found = 0;

    uint8_t header[BOARDROOM_SECTOR_BYTES];
    if (BoardroomRead(seat, header_lba, 1, header) != 0) {
        kprintf("[Ground] seat %u: sector %llu would not be read\n",
                seat, (unsigned long long)header_lba);
        return GPT_TABLE_DAMAGED;
    }

    if (memcmp(header + GPT_SIG_OFFSET, g_gpt_signature, 8) != 0) {
        return GPT_TABLE_ABSENT;
    }

    uint32_t header_bytes = le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES ||
        header_bytes > BOARDROOM_SECTOR_BYTES) {
        kprintf("[Ground] seat %u: the GPT header at sector %llu states a "
                "length of %u, which cannot be one\n",
                seat, (unsigned long long)header_lba, header_bytes);
        return GPT_TABLE_DAMAGED;
    }

    uint8_t  probe[BOARDROOM_SECTOR_BYTES];
    memcpy(probe, header, header_bytes);
    memset(probe + GPT_HEADER_CRC_OFFSET, 0, 4);
    uint32_t want_header_crc = le32(header + GPT_HEADER_CRC_OFFSET);
    uint32_t have_header_crc = KCrc32(probe, header_bytes);
    if (have_header_crc != want_header_crc) {
        kprintf("[Ground] seat %u: the GPT header at sector %llu does not "
                "match its own checksum (0x%08x against 0x%08x)\n",
                seat, (unsigned long long)header_lba,
                have_header_crc, want_header_crc);
        return GPT_TABLE_DAMAGED;
    }

    uint64_t my_lba = le64(header + GPT_MY_LBA_OFFSET);
    if (my_lba != header_lba) {
        kprintf("[Ground] seat %u: the GPT header at sector %llu says it "
                "lives at %llu — it is a copy of one from somewhere else\n",
                seat, (unsigned long long)header_lba,
                (unsigned long long)my_lba);
        return GPT_TABLE_DAMAGED;
    }

    uint64_t alt_lba = le64(header + GPT_ALT_LBA_OFFSET);
    if (medium_sectors != 0 && header_lba == GPT_HEADER_LBA &&
        alt_lba != medium_sectors - 1) {
        kprintf("[Ground] seat %u: its GPT keeps the far copy at sector %llu "
                "and the medium runs to %llu — this table was made for a "
                "smaller disk and copied onto this one\n",
                seat, (unsigned long long)alt_lba,
                (unsigned long long)(medium_sectors - 1));
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
        return GPT_TABLE_DAMAGED;
    }

    uint32_t array_bytes = entry_count * entry_bytes;
    uint32_t array_secs  = (array_bytes + BOARDROOM_SECTOR_BYTES - 1) /
                           BOARDROOM_SECTOR_BYTES;

    if (medium_sectors != 0 &&
        (entry_lba >= medium_sectors ||
         entry_lba + array_secs > medium_sectors)) {
        kprintf("[Ground] seat %u: its GPT puts the entry array at sectors "
                "%llu..%llu and the medium has %llu — not reading it\n",
                seat, (unsigned long long)entry_lba,
                (unsigned long long)(entry_lba + array_secs - 1),
                (unsigned long long)medium_sectors);
        return GPT_TABLE_DAMAGED;
    }

    uint8_t *array = (uint8_t *)kmalloc(array_secs * BOARDROOM_SECTOR_BYTES);
    if (!array) {
        kprintf("[Ground] seat %u: no memory to read its GPT entries\n", seat);
        return GPT_TABLE_DAMAGED;
    }

    GptVerdict verdict = GPT_TABLE_READ;

    if (BoardroomRead(seat, entry_lba, array_secs, array) != 0) {
        kprintf("[Ground] seat %u: would not give up its GPT entries\n", seat);
        verdict = GPT_TABLE_DAMAGED;
        goto done;
    }

    uint32_t have_array_crc = KCrc32(array, array_bytes);
    if (have_array_crc != want_array_crc) {
        kprintf("[Ground] seat %u: the GPT entries at sector %llu do not match "
                "their own checksum (0x%08x against 0x%08x)\n",
                seat, (unsigned long long)entry_lba,
                have_array_crc, want_array_crc);
        verdict = GPT_TABLE_DAMAGED;
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

        if (medium_sectors != 0 && last >= medium_sectors) {
            kprintf("[Ground] seat %u: GPT entry %u claims sectors %llu..%llu "
                    "and the medium has %llu — not using it\n",
                    seat, i, (unsigned long long)first,
                    (unsigned long long)last,
                    (unsigned long long)medium_sectors);
            continue;
        }

        if (!ground_room(*found, max, seat)) break;

        out[*found].start_sector = first;
        out[*found].sectors      = last - first + 1;
        out[*found].origin       = GROUND_FROM_GPT;
        out[*found].entry        = (uint8_t)i;
        (*found)++;
    }

done:
    kfree(array);
    if (verdict != GPT_TABLE_READ) {
        *found = 0;
    }
    return verdict;
}

static uint8_t ground_survey_gpt(uint8_t seat, MediumGround *out, uint8_t max)
{
    uint64_t medium = BoardroomSeatSectors(seat);
    uint8_t  found  = 0;

    GptVerdict v = gpt_read_one(seat, GPT_HEADER_LBA, medium, out, max, &found);
    if (v == GPT_TABLE_READ) {
        return found;
    }

    if (medium == 0) {
        kprintf("[Ground] seat %u: its GPT is damaged, and the medium will not "
                "say how far it runs — so the copy at the far end cannot be "
                "found\n", seat);
        return 0;
    }


    kprintf("[Ground] seat %u: its GPT %s — reading the copy at the far end, "
            "sector %llu (UEFI 2.10 5.3.2)\n", seat,
            (v == GPT_TABLE_ABSENT) ? "is not where the disk says it is"
                                    : "is damaged",
            (unsigned long long)(medium - 1));

    found = 0;
    v = gpt_read_one(seat, medium - 1, medium, out, max, &found);
    if (v == GPT_TABLE_READ) {
        kprintf("[Ground] seat %u: the copy at the far end is good — this disk "
                "is readable after all\n", seat);
        return found;
    }

    kprintf("[Ground] seat %u: the copy at the far end is %s — this disk has "
            "no table left to read\n", seat,
            (v == GPT_TABLE_ABSENT) ? "not there either" : "damaged too");
    return 0;
}

static uint8_t ground_survey_medium(uint8_t seat, MediumGround *out)
{
    const uint8_t max = GROUND_MAX_PER_MEDIUM;
    const uint64_t medium = BoardroomSeatSectors(seat);

    uint8_t sector0[BOARDROOM_SECTOR_BYTES];
    if (BoardroomRead(seat, 0, 1, sector0) != 0) {
        kprintf("[Ground] seat %u: could not be read at all\n", seat);
        return 0;
    }

    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        kprintf("[Ground] seat %u: no partition table on it\n", seat);
        return 0;
    }

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

        if (medium != 0 && (start >= medium || start + count > medium)) {
            kprintf("[Ground] seat %u: MBR entry %u claims sectors %llu..%llu "
                    "and the medium has %llu — not using it\n",
                    seat, i, (unsigned long long)start,
                    (unsigned long long)(start + count - 1),
                    (unsigned long long)medium);
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

uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max)
{
    if (!out || max == 0) return 0;

    MediumGround all[GROUND_MAX_PER_MEDIUM];
    uint8_t      claimed;

    uint32_t seating = BoardroomSeatSeating(seat);

    if (seating == 0 || !memo_recall(seat, seating, all, &claimed)) {
        claimed = ground_survey_medium(seat, all);
        if (seating != 0) {
            memo_keep(seat, seating, all, claimed);
        }
    }

    if (claimed > max) {
        kprintf("[Ground] seat %u carries %u runs of BoxOS ground and the "
                "survey it was asked for holds %u; the rest was not handed "
                "over\n", seat, claimed, max);
        claimed = max;
    }

    memcpy(out, all, (size_t)claimed * sizeof(MediumGround));
    return claimed;
}