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
 * ── What this seat's table said, so it is read once and not five times ──────
 *
 * The same medium's table is surveyed several times in one boot by parties
 * that have no business knowing about each other: the room describing what it
 * has just seated, a filesystem asking every chair whether its volume is
 * there, the mount standing on the ground it chose, and the boot survey
 * walking all of them. Measured on this machine — five reads of sector 0 for
 * one seat, in one boot. On a flash drive each of those is a transfer over the
 * bus, and on a GPT disk it is three.
 *
 * The answer is small, is owned by nobody — MediumGround is four plain numbers
 * — and cannot go stale while the medium stays put. So it is remembered per
 * chair, keyed by WHICH OCCUPANT of that chair it describes. The Boardroom
 * already has that fact and already keeps it for exactly this kind of
 * question: a chair whose seating has not changed has not changed hands. A
 * stick pulled out and pushed back gets a new seating and is read again, which
 * is right — it may not be the same stick and it may not have the same table.
 *
 * ‼ NOTHING IN THIS KERNEL WRITES A PARTITION TABLE, and this depends on that.
 * Every write to a medium goes through BoardroomWrite, and the only callers
 * are the volume — which addresses sectors from its own ground and therefore
 * cannot reach sector 0 of the medium — and Braid, which no running
 * configuration builds in. A writer of partition tables that ever appears MUST
 * drop the memo for the seat it wrote to, or the next survey will answer from
 * before the write.
 *
 * Two cores surveying the same chair cannot produce a wrong answer, only a
 * wasted read: an entry is only ever handed back when its key matches
 * EXACTLY, so an entry written under a seating that has since changed is never
 * matched again by anybody.
 *
 * One entry per chair, made when that chair is first surveyed and never given
 * back — the room's own list of chairs is kept the same way and for the same
 * reason. A fixed array would be a limit invented here rather than one the
 * hardware states, and freeing an entry would be freeing what the next survey
 * immediately asks for again.
 */
typedef struct GroundMemo {
    struct GroundMemo *next;
    uint8_t      seat;
    uint32_t     seating;       /* which occupant of that chair this describes */
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

    /* Allocated before the lock is taken and given back if it turns out not to
     * be needed. kmalloc under a spinlock is a wait of unbounded length inside
     * one of bounded length, and this is reached from the pass that seats media
     * arriving on the USB bus; the Boardroom's own seat_take is built the same
     * way for the same reason. */
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
            /* No memory for a memo is not a failure. It is a survey that will
             * be read off the medium again next time, which is what every
             * survey did before there were any. */
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

/*
 * The survey itself, with nothing remembered in it.
 *
 * Fills up to GROUND_MAX_PER_MEDIUM, which is this file's own ceiling and not
 * the caller's buffer: what is read off the medium is the whole of what the
 * medium says, so that the memo above holds a complete answer whoever asked
 * first. Trimming to what a particular caller can hold happens in one place,
 * in GroundSurvey.
 */
static uint8_t ground_survey_medium(uint8_t seat, MediumGround *out)
{
    const uint8_t max = GROUND_MAX_PER_MEDIUM;

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

uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max)
{
    if (!out || max == 0) return 0;

    MediumGround all[GROUND_MAX_PER_MEDIUM];
    uint8_t      claimed;

    /*
     * Zero means nobody has ever sat in that chair, or there is no such chair.
     * There is nothing to remember about an empty one, and a memo keyed on
     * zero could never be told apart from the next occupant's.
     */
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
