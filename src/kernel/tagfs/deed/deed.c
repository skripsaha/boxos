#include "deed.h"
#include "boardroom.h"
#include "klib.h"
#include "crypto.h"
#include "error.h"

/*
 * How much of the ground a Deed is allowed to occupy.
 *
 * Eight sectors is four kilobytes — one physical block on every medium in use,
 * so reading a Deed is one physical read and writing one is one physical
 * write, with no straddle either way. It is far more room than the stamps
 * defined today need, and a Deed that outgrows it is a Deed that has been
 * asked to carry something that belongs in the volume instead.
 */
#define DEED_SECTORS  8u
#define DEED_BYTES    (DEED_SECTORS * VOLUME_DEED_SECTOR_BYTES)

static const uint8_t g_deed_magic[8] = {
    VOLUME_DEED_MAGIC_0, VOLUME_DEED_MAGIC_1, VOLUME_DEED_MAGIC_2,
    VOLUME_DEED_MAGIC_3, VOLUME_DEED_MAGIC_4, VOLUME_DEED_MAGIC_5,
    VOLUME_DEED_MAGIC_6, VOLUME_DEED_MAGIC_7
};

static const char *deed_role_name(uint32_t role)
{
    switch (role) {
    case VOLUME_DEED_ROLE_HEAD: return "head";
    case VOLUME_DEED_ROLE_TAIL: return "tail";
    default:                    return "neither";
    }
}

/*
 * Everything that can be checked without knowing what the ground is for.
 *
 * The order matters: nothing is trusted until the checksum has passed, so the
 * only fields read before it are the ones needed to know how much to sum.
 * Those are bounded first, which is what stops a Deed with a stated length of
 * sixty thousand from being summed over memory it does not own.
 */
static error_t deed_validate(uint8_t seat, uint64_t at_sector,
                             const uint8_t *raw, uint32_t raw_bytes,
                             uint32_t want_role, DeedCopy *out)
{
    if (raw_bytes < sizeof(VolumeDeed)) return ERR_INVALID_ARGUMENT;

    VolumeDeed deed;
    memcpy(&deed, raw, sizeof(deed));

    /* Silent. "There is no deed at this sector" is the ordinary answer for
     * any ground that has not been made into a volume, and the caller knows
     * whether that is worth a line — a ground claimed for BoxOS with nothing
     * on it is news, a bare medium being looked at is not. Every OTHER refusal
     * below speaks, because each of those is a deed that is present and
     * wrong. */
    if (memcmp(deed.magic, g_deed_magic, sizeof(g_deed_magic)) != 0) {
        return ERR_FILE_NOT_FOUND;
    }

    if (deed.prologue_bytes < sizeof(VolumeDeed) ||
        deed.prologue_bytes > raw_bytes ||
        deed.stamp_bytes > raw_bytes - deed.prologue_bytes) {
        kprintf("[Deed] seat %u sector %llu: states a prologue of %u and %u "
                "bytes of stamps, which do not fit the %u it lives in\n",
                seat, (unsigned long long)at_sector,
                deed.prologue_bytes, deed.stamp_bytes, raw_bytes);
        return ERR_INVALID_ARGUMENT;
    }

    /* The checksum covers the prologue with its own field zeroed, then exactly
     * the stamps — so it is taken over what the deed says it is, never over
     * the padding that follows it to the end of the sector. Summed from a copy
     * so the buffer still holds what the medium holds. */
    uint32_t summed_bytes = (uint32_t)deed.prologue_bytes + deed.stamp_bytes;
    uint8_t *probe = (uint8_t *)kmalloc(summed_bytes);
    if (!probe) return ERR_NO_MEMORY;
    memcpy(probe, raw, summed_bytes);
    memset(probe + __builtin_offsetof(VolumeDeed, crc32), 0, sizeof(uint32_t));
    uint32_t have = KCrc32(probe, summed_bytes);
    kfree(probe);

    if (have != deed.crc32) {
        kprintf("[Deed] seat %u sector %llu: does not match its own checksum "
                "(0x%08x against 0x%08x)\n",
                seat, (unsigned long long)at_sector, have, deed.crc32);
        return ERR_CORRUPTED;
    }

    if (deed.role != want_role) {
        kprintf("[Deed] seat %u sector %llu: this is the %s of a volume and "
                "the %s was asked for — the read went to the wrong place\n",
                seat, (unsigned long long)at_sector,
                deed_role_name(deed.role), deed_role_name(want_role));
        return ERR_INVALID_ARGUMENT;
    }

    out->head      = deed;
    out->stamps    = raw + deed.prologue_bytes;
    out->raw       = (uint8_t *)raw;
    out->raw_bytes = raw_bytes;
    return OK;
}

static error_t deed_read_at(uint8_t seat, uint64_t sector, uint32_t want_role,
                            DeedCopy *out)
{
    uint8_t *raw = (uint8_t *)kmalloc(DEED_BYTES);
    if (!raw) return ERR_NO_MEMORY;

    if (BoardroomRead(seat, sector, DEED_SECTORS, raw) != 0) {
        kfree(raw);
        return ERR_IO;
    }

    error_t rc = deed_validate(seat, sector, raw, DEED_BYTES, want_role, out);
    if (rc != OK) {
        kfree(raw);
        out->raw = NULL;
    }
    return rc;
}

error_t DeedReadHead(uint8_t seat, const MediumGround *ground, DeedCopy *out)
{
    if (!ground || !out) return ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (ground->sectors < DEED_SECTORS) return ERR_INVALID_ARGUMENT;

    error_t rc = deed_read_at(seat, ground->start_sector,
                              VOLUME_DEED_ROLE_HEAD, out);
    if (rc != OK) return rc;

    /*
     * The volume claims a length; the ground it was handed has one. A volume
     * longer than its ground is an image copied short — the case that mounts
     * happily today and then reads into somebody else's partition, or off the
     * end of the medium entirely.
     *
     * A volume SHORTER than its ground is not an error: a partition may have
     * been made larger than the volume that was put in it, and the volume
     * simply does not use the rest.
     */
    if (out->head.sectors > ground->sectors) {
        kprintf("[Deed] seat %u: the volume claims %llu sectors and the ground "
                "it stands on has %llu — this is not all of it\n",
                seat, (unsigned long long)out->head.sectors,
                (unsigned long long)ground->sectors);
        DeedRelease(out);
        return ERR_CORRUPTED;
    }

    if (out->head.tail_sector == 0 ||
        out->head.tail_sector + DEED_SECTORS > out->head.sectors) {
        kprintf("[Deed] seat %u: it puts its own far copy at sector %llu, "
                "which is not inside itself\n",
                seat, (unsigned long long)out->head.tail_sector);
        DeedRelease(out);
        return ERR_CORRUPTED;
    }

    return OK;
}

error_t DeedReadTail(uint8_t seat, const MediumGround *ground,
                     const DeedCopy *head, DeedCopy *out)
{
    if (!ground || !head || !out) return ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    uint64_t at = ground->start_sector + head->head.tail_sector;
    error_t rc = deed_read_at(seat, at, VOLUME_DEED_ROLE_TAIL, out);
    if (rc != OK) return rc;

    /* Two copies of one volume have to be the same volume. Anything else is
     * two volumes overlapping, which is worse than either being damaged. */
    if (memcmp(out->head.uuid, head->head.uuid, 16) != 0 ||
        out->head.sectors != head->head.sectors) {
        kprintf("[Deed] seat %u: the copy at the far end belongs to a "
                "different volume than the one at the near end\n", seat);
        DeedRelease(out);
        return ERR_CORRUPTED;
    }

    return OK;
}

void DeedRelease(DeedCopy *copy)
{
    if (!copy || !copy->raw) return;
    kfree(copy->raw);
    copy->raw    = NULL;
    copy->stamps = NULL;
}

const void *DeedStamp(const DeedCopy *copy, uint16_t kind, uint16_t *out_bytes)
{
    if (!copy || !copy->stamps) return NULL;

    uint32_t left = copy->head.stamp_bytes;
    const uint8_t *p = copy->stamps;

    while (left >= sizeof(VolumeStamp)) {
        VolumeStamp s;
        memcpy(&s, p, sizeof(s));

        uint32_t payload = s.bytes;
        if (payload > left - sizeof(VolumeStamp)) {
            /* A stamp that claims more than remains ends the walk. Said once,
             * here, rather than by every caller that goes looking. */
            kprintf("[Deed] a stamp of kind %u claims %u bytes with %u left — "
                    "the rest of this deed is not read\n",
                    s.kind, s.bytes, (unsigned)(left - sizeof(VolumeStamp)));
            return NULL;
        }

        if (s.kind == kind) {
            if (out_bytes) *out_bytes = s.bytes;
            return p + sizeof(VolumeStamp);
        }

        /* The next stamp starts at the next four-byte boundary after this
         * one's payload — the same rule the Boarding Pass walks by. */
        uint32_t step = sizeof(VolumeStamp) + payload;
        step = (step + 3u) & ~3u;
        if (step > left) return NULL;
        p    += step;
        left -= step;
    }

    return NULL;
}

void DeedDescribe(uint8_t seat, const DeedCopy *copy)
{
    if (!copy) return;

    char text[33];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        text[i * 2]     = hex[(copy->head.uuid[i] >> 4) & 0xF];
        text[i * 2 + 1] = hex[copy->head.uuid[i] & 0xF];
    }
    text[32] = '\0';

    kprintf("[Deed] seat %u: volume %s, %llu sectors, far copy at %llu\n",
            seat, text, (unsigned long long)copy->head.sectors,
            (unsigned long long)copy->head.tail_sector);

    uint16_t bytes = 0;
    const VolumeGeometry *geo =
        (const VolumeGeometry *)DeedStamp(copy, VOLUME_STAMP_GEOMETRY, &bytes);
    if (geo && bytes >= sizeof(*geo)) {
        kprintf("[Deed] seat %u: laid out for %u-byte blocks on a medium of "
                "%u-byte physical ones, aligned to %u\n",
                seat, geo->block_bytes, geo->physical_bytes, geo->grain_bytes);
    }

    const VolumeBorn *born =
        (const VolumeBorn *)DeedStamp(copy, VOLUME_STAMP_BORN, &bytes);
    if (born && bytes >= sizeof(*born)) {
        char maker[17];
        memcpy(maker, born->maker, 16);
        maker[16] = '\0';
        kprintf("[Deed] seat %u: made by %s\n", seat, maker);
    }
}

void DeedSurveyAll(void)
{
    uint8_t seats = BoardroomSeatCount();

    for (uint8_t seat = 0; seat < seats; seat++) {
        if (!BoardroomSeatOccupied(seat)) continue;

        MediumGround ground[GROUND_MAX_PER_MEDIUM];
        uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM);

        for (uint8_t g = 0; g < claimed; g++) {
            DeedCopy head;
            if (DeedReadHead(seat, &ground[g], &head) != OK) {
                kprintf("[Deed] seat %u ground %u is claimed for BoxOS and "
                        "carries no deed this kernel can read\n", seat, g);
                continue;
            }

            DeedDescribe(seat, &head);

            /* And whether the whole of it is there. A volume with only a head
             * still mounts — a medium may have lost its tail — but it is a
             * volume with no second opinion left, and that is worth a line. */
            DeedCopy tail;
            if (DeedReadTail(seat, &ground[g], &head, &tail) == OK) {
                kprintf("[Deed] seat %u: its far copy agrees — the whole "
                        "volume is present\n", seat);
                DeedRelease(&tail);
            } else {
                kprintf("[Deed] seat %u: no readable copy at the far end; "
                        "this volume has one deed and no second opinion\n",
                        seat);
            }

            DeedRelease(&head);
        }
    }
}
