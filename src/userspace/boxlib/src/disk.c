#include "box/disk.h"
#include "box/file.h"
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
            uint16_t len, loaders;
            if (at + DISK_PART_LISTED_HEAD > bytes) return 0;
            memcpy(&len, records + at + 40, 2);
            memcpy(&loaders, records + at + 60, 2);
            at   += DISK_PART_LISTED_HEAD + len + loaders;
            text += len + 1u + loaders;
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
        memcpy(d->Guid, r + 32, 16);
        d->Name = pen;
        memcpy(pen, r + DISK_LISTED_HEAD, name_len);
        pen += name_len;
        *pen++ = '\0';
        at += DISK_LISTED_HEAD + name_len;

        d->PartCount = p;
        d->Parts     = part;
        for (uint16_t k = 0; k < p; k++, part++) {
            const uint8_t *e = records + at;
            uint16_t       len, loaders;
            part->Kind          = e[0];
            part->Mounted       = (e[1] & DISK_PART_MOUNTED) != 0;
            part->DefaultLoader = (e[1] & DISK_PART_DEFAULT_LOADER) != 0;
            part->MoreLoaders   = (e[1] & DISK_PART_MORE_LOADERS) != 0;
            memcpy(&part->Entry, e + 2, 2);
            memcpy(&part->Volume, e + 4, 4);
            memcpy(&part->First, e + 8, 8);
            memcpy(&part->Blocks, e + 16, 8);
            memcpy(part->Type, e + 24, 16);
            memcpy(&len, e + 40, 2);
            memcpy(part->Unique, e + 42, 16);
            part->Holds = e[58];
            memcpy(&loaders, e + 60, 2);
            part->Name = pen;
            memcpy(pen, e + DISK_PART_LISTED_HEAD, len);
            pen += len;
            *pen++ = '\0';
            /* The loaders, each NUL-ended as the record has them. */
            memcpy(pen, e + DISK_PART_LISTED_HEAD + len, loaders);
            part->LoaderCount = 0;
            for (uint16_t b = 0; b < loaders && part->LoaderCount < DISK_LOADERS_MOST;) {
                part->Loaders[part->LoaderCount++] = pen + b;
                while (b < loaders && pen[b]) b++;
                b++;
            }
            if (loaders && pen[loaders - 1u]) part->LoaderCount = 0;    /* a list that does not end is none */
            pen += loaders;
            at += DISK_PART_LISTED_HEAD + len + loaders;
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

/* Room for one disk's record as tables are made: its fixed part and name, an
 * ESP's loaders, and the 128 entries every partitioning tool writes, each
 * named, with a free run past each. A table of more is room asked again. */
#define DISK_RECORD_COMMON (DISK_LISTED_HEAD + DISK_NAME_MOST + DISK_LOADERS_MOST * (DISK_LOADER_NAME_MOST + 1u) + \
                            128u * (DISK_PART_LISTED_HEAD + DISK_NAME_MOST + DISK_FREE_LISTED))
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

const char *disk_loader_shown(const char *dir)
{
    static const struct {
        const char *dir;
        const char *system;
    } known[] = {
        { "Microsoft", "Windows" }, { "ubuntu", "Ubuntu" },    { "debian", "Debian" },   { "fedora", "Fedora" },
        { "opensuse", "openSUSE" }, { "redhat", "Red Hat" },    { "centos", "CentOS" },   { "rocky", "Rocky Linux" },
        { "almalinux", "AlmaLinux" }, { "arch", "Arch Linux" }, { "manjaro", "Manjaro" }, { "pop", "Pop!_OS" },
        { "linuxmint", "Linux Mint" }, { "elementary", "elementary OS" }, { "neon", "KDE neon" },
        { "systemd", "systemd-boot" }, { "refind", "rEFInd" },  { "grub", "GRUB" },       { "Apple", "macOS" },
        { "BoxOS", "BoxOS" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        const char *a = dir, *b = known[i].dir;
        while (*a && *b && ((*a | 32) == (*b | 32))) a++, b++;
        if (!*a && !*b) return known[i].system;
    }
    return dir;
}

size_t disk_named(const char **text, const DiskListed *disks, size_t count, const DiskListed **out)
{
    if (!text || !*text || !out) return 0;
    VolumeListed *volumes      = NULL;
    size_t        volume_count = 0;
    if (volume_list(&volumes, &volume_count) != 0) volume_count = 0;
    size_t best = 0, called = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = name_begins(*text, disks[i].Name);
        for (size_t p = 0; p < disks[i].PartCount; p++)
            for (size_t v = 0; v < volume_count; v++)
                if (disks[i].Parts[p].Volume == volumes[v].Number + 1u) {
                    size_t m = name_begins(*text, volumes[v].Name);
                    n        = m > n ? m : n;
                }
        if (n > best) {
            best   = n;
            called = 0;
        }
        if (n && n == best && called++ == 0) *out = &disks[i];
    }
    free(volumes);
    if (!best) {
        const char *at   = *text;
        uint32_t    seat = 0;
        while (*at >= '0' && *at <= '9' && seat <= 0xFFu) seat = seat * 10u + (uint32_t)(*at++ - '0');
        if (at != *text && (*at == ' ' || *at == '\0'))
            for (size_t i = 0; i < count; i++)
                if (disks[i].Seat == seat) {
                    *out   = &disks[i];
                    best   = (size_t)(at - *text);
                    called = 1;
                }
    }
    if (called) {
        *text += best;
        while (**text == ' ') (*text)++;
    }
    return called;
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

int disk_boot(const DiskListed *disk, uint16_t entry, uint32_t volume, bool first, DiskBooted *booted)
{
    uint8_t more[7], answer[5] = { 0 };
    memcpy(more, &entry, 2);
    memcpy(more + 2, &volume, 4);
    more[6] = first ? 1u : 0u;
    int rc  = disk_changed(HW_DISK_BOOT, disk, more, sizeof(more), answer, sizeof(answer));
    if (rc == 0 && booted)
        *booted = (DiskBooted){ .Option = (uint16_t)(answer[0] | answer[1] << 8), .Given = answer[2] != 0,
                                .Fallback = answer[3] != 0, .Bios = answer[4] != 0 };
    return rc;
}

int disk_grow(const DiskListed *disk, uint16_t entry, uint64_t blocks, uint64_t *grown)
{
    uint8_t  more[10];
    uint64_t now = 0;
    memcpy(more, &entry, 2);
    memcpy(more + 2, &blocks, 8);
    int rc = disk_changed(HW_DISK_GROW, disk, more, sizeof(more), &now, sizeof(now));
    if (rc == 0 && grown) *grown = now;
    return rc;
}

int volume_lay(const DiskListed *disk, uint16_t entry, const char *name, uint32_t *number, uint32_t *roles)
{
    size_t  len = name ? strlen(name) : 0;
    uint8_t params[9 + STORAGE_VOLUME_NAME_MOST];
    uint8_t answer[8] = { 0 };
    if (!disk || len > STORAGE_VOLUME_NAME_MOST) return -ERR_INVALID_ARGUMENT;
    uint16_t len16 = (uint16_t)len;
    params[0] = disk->Seat;
    memcpy(params + 1, &disk->Seating, 4);
    memcpy(params + 5, &entry, 2);
    memcpy(params + 7, &len16, 2);
    if (len) memcpy(params + 9, name, len);
    int rc = MfCall1(DECK_STORAGE, STORAGE_VOLUME_LAY, params, (uint32_t)(9u + len), NULL, 0, answer,
                     sizeof(answer), NULL, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (number) memcpy(number, answer, 4);
    if (roles) memcpy(roles, answer + 4, 4);
    return 0;
}
