#include "box/disk.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/core/manifest.h"
#include "box/timeouts.h"   /* BOX_ANSWER_GUARANTEED */
#include "boxos_decks.h"

/* =========================================================================
 *  DISKS — the machine's, and their tables (box/disk.h)
 * ========================================================================= */

/* Bytes a list of `count` disks takes as DiskListed, their partitions and free
 * runs and every name NUL-ended after them; 0 for a list that does not hold. */
static size_t disks_measured(const uint8_t *records, size_t bytes, uint32_t count, size_t *parts, size_t *runs)
{
    size_t at = 0, text = 0;
    *parts = *runs = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t p, f, name_len;
        if (at + DISK_LISTED_HEAD > bytes) return 0;
        memcpy(&p, records + at + 26, 2);
        memcpy(&f, records + at + 28, 2);
        memcpy(&name_len, records + at + 30, 2);
        at   += DISK_LISTED_HEAD + name_len;
        text += name_len + 1u;
        for (uint16_t k = 0; k < p; k++) {
            uint16_t len;
            if (at + DISK_PART_LISTED_HEAD > bytes) return 0;
            memcpy(&len, records + at + 40, 2);
            at   += DISK_PART_LISTED_HEAD + len;
            text += len + 1u;
        }
        at += (size_t)f * DISK_FREE_LISTED;
        if (at > bytes) return 0;
        *parts += p;
        *runs  += f;
    }
    return count * sizeof(DiskListed) + *parts * sizeof(DiskPartListed) + *runs * sizeof(DiskFreeListed) + text;
}

/* The records of a list laid out as DiskListed, in one block. */
static int disks_laid(const uint8_t *records, size_t bytes, uint32_t count, DiskListed **out)
{
    size_t parts, runs;
    size_t size = disks_measured(records, bytes, count, &parts, &runs);
    if (count && !size) return -ERR_INTERNAL;
    DiskListed *list = malloc(size ? size : 1u);
    if (!list) return -ERR_NO_MEMORY;
    DiskPartListed *part = (DiskPartListed *)(list + count);
    DiskFreeListed *run  = (DiskFreeListed *)(part + parts);
    char           *pen  = (char *)(run + runs);

    size_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = records + at;
        DiskListed    *d = &list[i];
        uint16_t       p, f, name_len;
        d->Seat          = r[0];
        d->Board         = r[1];
        d->Removable     = (r[2] & DISK_REMOVABLE) != 0;
        d->RefusesWrites = (r[2] & DISK_REFUSES_WRITES) != 0;
        d->CarriesHouse  = (r[2] & DISK_CARRIES_HOUSE) != 0;
        d->Table         = r[3];
        memcpy(&d->Seating, r + 4, 4);
        memcpy(&d->BlockBytes, r + 8, 4);
        memcpy(&d->PhysicalBytes, r + 12, 4);
        memcpy(&d->Blocks, r + 16, 8);
        d->Copies = r[24];
        memcpy(&p, r + 26, 2);
        memcpy(&f, r + 28, 2);
        memcpy(&name_len, r + 30, 2);
        d->Name = pen;
        memcpy(pen, r + DISK_LISTED_HEAD, name_len);
        pen += name_len;
        *pen++ = '\0';
        at += DISK_LISTED_HEAD + name_len;

        d->PartCount = p;
        d->Parts     = part;
        for (uint16_t k = 0; k < p; k++, part++) {
            const uint8_t *e = records + at;
            uint16_t       len;
            part->Kind    = e[0];
            part->Mounted = (e[1] & DISK_PART_MOUNTED) != 0;
            memcpy(&part->Entry, e + 2, 2);
            memcpy(&part->Volume, e + 4, 4);
            memcpy(&part->First, e + 8, 8);
            memcpy(&part->Blocks, e + 16, 8);
            memcpy(part->Type, e + 24, 16);
            memcpy(&len, e + 40, 2);
            part->Name = pen;
            memcpy(pen, e + DISK_PART_LISTED_HEAD, len);
            pen += len;
            *pen++ = '\0';
            at += DISK_PART_LISTED_HEAD + len;
        }
        d->FreeCount = f;
        d->Free      = run;
        for (uint16_t k = 0; k < f; k++, run++) {
            memcpy(&run->First, records + at, 8);
            memcpy(&run->Blocks, records + at + 8, 8);
            at += DISK_FREE_LISTED;
        }
    }
    *out = list;
    return 0;
}

/* Room for one disk's record as tables are made: its fixed part and name, and
 * the 128 entries every partitioning tool writes, each named, with a free run
 * past each. A table of more is room asked again. */
#define DISK_RECORD_COMMON (DISK_LISTED_HEAD + DISK_NAME_MOST + 128u * (DISK_PART_LISTED_HEAD + DISK_NAME_MOST + \
                                                                        DISK_FREE_LISTED))
/* The largest table the kernel reads holds eight times that many entries. */
#define DISK_RECORD_ROOMS_MOST 8u

int disk_list(DiskListed **out, size_t *count)
{
    if (!out || !count) return -ERR_INVALID_ARGUMENT;
    *out   = NULL;
    *count = 0;
    /* Asked again, with twice the room, while the answer counted more disks
     * than it gave — the disks themselves, or one table larger than room for
     * them all; never past what the largest tables of every disk take. */
    for (uint32_t room = 2;;) {
        size_t   size = 8u + (size_t)room * DISK_RECORD_COMMON;
        uint8_t *page = malloc(size);
        if (!page) return -ERR_NO_MEMORY;
        int rc = MfCall1(DECK_HARDWARE, HW_DISKS_LIST, NULL, 0, NULL, 0, page, (uint32_t)size, NULL,
                         BOX_ANSWER_GUARANTEED, NULL);
        if (rc != 0) {
            free(page);
            return box_fail(rc);
        }
        uint32_t delivered, total;
        memcpy(&delivered, page, 4);
        memcpy(&total, page + 4, 4);
        if (delivered < total) {
            free(page);
            if (room >= (total + 1u) * DISK_RECORD_ROOMS_MOST) return -ERR_BUFFER_TOO_SMALL;
            room = room * 2u > total + 1u ? room * 2u : total + 1u;
            continue;
        }
        rc = disks_laid(page + 8, size - 8, delivered, out);
        free(page);
        if (rc == 0) *count = delivered;
        return rc;
    }
}

/* One change of a disk's table: [u8 seat][u32 seating], then `more`. */
static int disk_changed(uint16_t op, const DiskListed *disk, const void *more, uint32_t more_bytes, void *answer,
                        uint32_t answer_bytes)
{
    uint8_t params[5 + 17];
    if (!disk || more_bytes > sizeof(params) - 5) return -ERR_INVALID_ARGUMENT;
    params[0] = disk->Seat;
    memcpy(params + 1, &disk->Seating, 4);
    if (more_bytes) memcpy(params + 5, more, more_bytes);
    int rc = MfCall1(DECK_HARDWARE, op, params, 5u + more_bytes, NULL, 0, answer, answer_bytes, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return rc != 0 ? box_fail(rc) : 0;
}

int disk_lay(const DiskListed *disk)
{
    return disk_changed(HW_DISK_LAY, disk, NULL, 0, NULL, 0);
}

int disk_add(const DiskListed *disk, uint8_t kind, uint64_t first, uint64_t blocks, uint16_t *entry)
{
    uint8_t  more[17];
    uint16_t took = 0;
    more[0] = kind;
    memcpy(more + 1, &first, 8);
    memcpy(more + 9, &blocks, 8);
    int rc = disk_changed(HW_DISK_ADD, disk, more, sizeof(more), &took, sizeof(took));
    if (rc == 0 && entry) *entry = took;
    return rc;
}

int disk_remove(const DiskListed *disk, uint16_t entry)
{
    return disk_changed(HW_DISK_REMOVE, disk, &entry, sizeof(entry), NULL, 0);
}

int disk_mend(const DiskListed *disk)
{
    return disk_changed(HW_DISK_MEND, disk, NULL, 0, NULL, 0);
}
