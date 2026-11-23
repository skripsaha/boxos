#include "disk_table.h"
#include "disk_table_internal.h"
#include "boardroom.h"
#include "klib.h"
#include "crypto.h"
#include "atomics.h"

/*
 * The reading of a disk's table (disk_table.h). The writing is
 * disk_table_write.c; what both must agree on — the shape of a GPT on the
 * medium — is disk_table_internal.h.
 */

/* ── MBR ─────────────────────────────────────────────────────────────────
 * Sector 0. Four entries of sixteen bytes at offset 446, then 0x55 0xAA: the
 * type byte at 4, the first block and the count at 8 and 12, little-endian
 * 32-bit, in the medium's own blocks. An extended entry is a chain of further
 * tables, each in the first block of its own run: its entry 0 one logical
 * partition counted from that block, its entry 1 the next table counted from
 * the extended entry's start.
 * ──────────────────────────────────────────────────────────────────────── */
#define MBR_TABLE_OFFSET     446u
#define MBR_ENTRY_BYTES      16u
#define MBR_ENTRY_COUNT      4u
#define MBR_ENTRY_TYPE       4u
#define MBR_ENTRY_START      8u
#define MBR_ENTRY_COUNT_AT   12u
/* A chain of extended tables longer than this is one that loops or lies. */
#define MBR_LOGICAL_MOST     64u

static uint16_t Le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t DiskLe32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t DiskLe64(const uint8_t* p)
{
    return (uint64_t)DiskLe32(p) | ((uint64_t)DiskLe32(p + 4) << 32);
}

uint64_t DiskTableGrain(const DiskTable* t)
{
    return DiskGrainBlocks(t->block_bytes, t->physical_bytes);
}

/* The medium's blocks [block, block + count) into `buf`. */
int DiskReadBlocks(uint8_t seat, uint32_t block_bytes, uint64_t block, uint64_t count, void* buf)
{
    uint64_t ratio = block_bytes / BOARDROOM_SECTOR_BYTES;
    return BoardroomRead(seat, block * ratio, (uint32_t)(count * ratio), buf);
}

/* ── one copy of a GPT ────────────────────────────────────────────────── */

void DiskGptCopyLetGo(DiskGptCopy* c)
{
    kfree(c->array);
    c->array = NULL;
}

/*
 * Read the copy whose header lies in block `header_block` and say what came of
 * it. Both checksums are verified: a header that holds can still point at an
 * array a write was cut in the middle of. `blocks` is how far the medium runs,
 * or 0 when it would not say; every bound is then the arithmetic's alone.
 */
DiskGptVerdict DiskGptCopyRead(uint8_t seat, uint32_t block_bytes, uint64_t blocks, uint64_t header_block,
                               DiskGptCopy* out)
{
    memset(out, 0, sizeof(*out));
    out->header_block = header_block;

    uint8_t* header = (uint8_t*)kmalloc(block_bytes);
    if (!header) {
        kprintf("[DiskTable] seat %u: no memory to read its GPT header\n", seat);
        return DISK_GPT_DAMAGED;
    }

    DiskGptVerdict verdict = DISK_GPT_READ;
    uint64_t       sector  = header_block * (block_bytes / BOARDROOM_SECTOR_BYTES);
    if (DiskReadBlocks(seat, block_bytes, header_block, 1, header) != 0) {
        kprintf("[DiskTable] seat %u: sector %llu would not be read\n", seat, (unsigned long long)sector);
        verdict = DISK_GPT_REFUSED;
        goto done;
    }
    if (memcmp(header + GPT_SIG_OFFSET, "EFI PART", 8) != 0) {
        verdict = DISK_GPT_ABSENT;
        goto done;
    }

    uint32_t header_bytes = DiskLe32(header + GPT_HEADER_SIZE_OFFSET);
    /* UEFI 2.10 §5.3.2: at least 92 and at most the logical block. */
    if (header_bytes < GPT_HEADER_MIN_BYTES || header_bytes > block_bytes) {
        kprintf("[DiskTable] seat %u: the GPT header at sector %llu states a length of %u, which cannot be one\n",
                seat, (unsigned long long)sector, header_bytes);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }
    uint32_t want = DiskLe32(header + GPT_HEADER_CRC_OFFSET);
    memset(header + GPT_HEADER_CRC_OFFSET, 0, 4);
    uint32_t have = KCrc32(header, header_bytes);
    if (have != want) {
        kprintf("[DiskTable] seat %u: the GPT header at sector %llu does not match its own checksum "
                "(0x%08x against 0x%08x)\n", seat, (unsigned long long)sector, have, want);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }

    /* ‼ A header says which block it lives in: a copy put somewhere else — a
     * primary duplicated into the last block, or a table of a 4Kn medium read
     * as though its blocks were 512 bytes — gives itself away here. */
    uint64_t my = DiskLe64(header + GPT_MY_LBA_OFFSET);
    if (my != header_block) {
        kprintf("[DiskTable] seat %u: the GPT header at sector %llu is block %llu of %u bytes and says it lives at "
                "block %llu — it is a copy of one from somewhere else, or from a medium addressed differently\n",
                seat, (unsigned long long)sector, (unsigned long long)header_block, block_bytes,
                (unsigned long long)my);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }

    out->alt_block    = DiskLe64(header + GPT_ALT_LBA_OFFSET);
    out->first_usable = DiskLe64(header + GPT_FIRST_USABLE_OFFSET);
    out->last_usable  = DiskLe64(header + GPT_LAST_USABLE_OFFSET);
    memcpy(out->disk_guid, header + GPT_DISK_GUID_OFFSET, 16);
    out->entry_block  = DiskLe64(header + GPT_ENTRY_LBA_OFFSET);
    out->entry_count  = DiskLe32(header + GPT_ENTRY_COUNT_OFFSET);
    out->entry_bytes  = DiskLe32(header + GPT_ENTRY_BYTES_OFFSET);
    out->array_crc    = DiskLe32(header + GPT_ENTRY_CRC_OFFSET);

    /* SizeOfPartitionEntry is 128 × 2^n (UEFI 2.10 §5.3.2), and it is the
     * stride the array is walked by. */
    if (out->entry_bytes < GPT_ENTRY_MIN_BYTES || (out->entry_bytes & (out->entry_bytes - 1u)) != 0u ||
        out->entry_count == 0 || out->entry_bytes > GPT_ARRAY_MAX_BYTES ||
        out->entry_count > GPT_ARRAY_MAX_BYTES / out->entry_bytes) {
        kprintf("[DiskTable] seat %u: its GPT states %u entries of %u bytes, which is not a table this reads\n",
                seat, out->entry_count, out->entry_bytes);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }
    if (out->first_usable > out->last_usable || (blocks && out->last_usable >= blocks)) {
        kprintf("[DiskTable] seat %u: its GPT says blocks %llu..%llu may be used, and the medium has %llu\n",
                seat, (unsigned long long)out->first_usable, (unsigned long long)out->last_usable,
                (unsigned long long)blocks);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }

    uint32_t array_bytes  = out->entry_count * out->entry_bytes;
    uint64_t array_blocks = (array_bytes + block_bytes - 1u) / block_bytes;
    if (out->entry_block > (~(uint64_t)0) / (block_bytes / BOARDROOM_SECTOR_BYTES) - array_blocks ||
        (blocks && out->entry_block + array_blocks > blocks)) {
        kprintf("[DiskTable] seat %u: its GPT puts the entry array at block %llu, past the medium's %llu\n",
                seat, (unsigned long long)out->entry_block, (unsigned long long)blocks);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }

    out->array = (uint8_t*)kmalloc(array_blocks * block_bytes);
    if (!out->array) {
        kprintf("[DiskTable] seat %u: no memory to read its GPT entries\n", seat);
        verdict = DISK_GPT_DAMAGED;
        goto done;
    }
    if (DiskReadBlocks(seat, block_bytes, out->entry_block, array_blocks, out->array) != 0) {
        kprintf("[DiskTable] seat %u: would not give up its GPT entries\n", seat);
        verdict = DISK_GPT_REFUSED;
        goto done;
    }
    have = KCrc32(out->array, array_bytes);
    if (have != out->array_crc) {
        kprintf("[DiskTable] seat %u: the GPT entries at block %llu do not match their own checksum "
                "(0x%08x against 0x%08x)\n", seat, (unsigned long long)out->entry_block, have, out->array_crc);
        verdict = DISK_GPT_DAMAGED;
    }

done:
    kfree(header);
    if (verdict != DISK_GPT_READ)
        DiskGptCopyLetGo(out);
    out->verdict = verdict;
    return verdict;
}

/* Whether two copies that both hold say the same table. */
static bool DiskGptCopiesAgree(const DiskGptCopy* a, const DiskGptCopy* b)
{
    return memcmp(a->disk_guid, b->disk_guid, 16) == 0 && a->first_usable == b->first_usable &&
           a->last_usable == b->last_usable && a->entry_count == b->entry_count &&
           a->entry_bytes == b->entry_bytes && a->array_crc == b->array_crc;
}

/* ── the partitions ───────────────────────────────────────────────────── */

/* A GPT name — 36 UTF-16LE units, ending at the first 0 — as UTF-8; a unit
 * no character is made of becomes U+FFFD. */
static void DiskNameOf(const uint8_t* units, char* out)
{
    size_t at = 0;
    for (uint32_t i = 0; i < GPT_ENTRY_NAME_UNITS; i++) {
        uint32_t c = Le16(units + 2u * i);
        if (c == 0)
            break;
        if (c >= 0xD800u && c < 0xDC00u && i + 1 < GPT_ENTRY_NAME_UNITS) {
            uint32_t low = Le16(units + 2u * (i + 1));
            if (low >= 0xDC00u && low < 0xE000u) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (low - 0xDC00u);
                i++;
            } else {
                c = 0xFFFDu;
            }
        } else if (c >= 0xD800u && c < 0xE000u) {
            c = 0xFFFDu;
        }
        if (c < 0x80u) {
            out[at++] = (char)c;
        } else if (c < 0x800u) {
            out[at++] = (char)(0xC0u | (c >> 6));
            out[at++] = (char)(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            out[at++] = (char)(0xE0u | (c >> 12));
            out[at++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[at++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            out[at++] = (char)(0xF0u | (c >> 18));
            out[at++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            out[at++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            out[at++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    out[at] = '\0';
}

static int DiskPartOrder(const void* a, const void* b, void* ctx)
{
    (void)ctx;
    const DiskPart* x = (const DiskPart*)a;
    const DiskPart* y = (const DiskPart*)b;
    return x->first < y->first ? -1 : x->first > y->first ? 1 : 0;
}

/* The partitions of the GPT `c` holds, in the order of their first block. */
static error_t DiskGptParts(DiskTable* t, const DiskGptCopy* c)
{
    static const uint8_t unused[16] = { 0 };
    uint32_t used = 0;
    for (uint32_t i = 0; i < c->entry_count; i++)
        used += memcmp(c->array + (uint64_t)i * c->entry_bytes, unused, 16) != 0;
    t->parts = used ? (DiskPart*)kmalloc(used * sizeof(DiskPart)) : NULL;
    if (used && !t->parts)
        return ERR_NO_MEMORY;

    for (uint32_t i = 0; i < c->entry_count; i++) {
        const uint8_t* e = c->array + (uint64_t)i * c->entry_bytes;
        if (memcmp(e + GPT_ENTRY_TYPE_OFFSET, unused, 16) == 0)
            continue;
        uint64_t first = DiskLe64(e + GPT_ENTRY_FIRST_OFFSET);
        uint64_t last  = DiskLe64(e + GPT_ENTRY_LAST_OFFSET);
        /* ‼ A partition has to lie on the disk, inside what the table says may
         * be used: one that does not is no place anybody can stand. */
        if (last < first || first < c->first_usable || last > c->last_usable) {
            kprintf("[DiskTable] seat %u: GPT entry %u claims blocks %llu..%llu, outside %llu..%llu the table "
                    "may use — not counted\n", t->seat, i, (unsigned long long)first, (unsigned long long)last,
                    (unsigned long long)c->first_usable, (unsigned long long)c->last_usable);
            continue;
        }
        DiskPart* p = &t->parts[t->part_count++];
        p->entry      = (uint16_t)i;
        memcpy(p->type, e + GPT_ENTRY_TYPE_OFFSET, 16);
        memcpy(p->unique, e + GPT_ENTRY_UNIQUE_OFFSET, 16);
        p->kind       = DiskKindOfType(p->type);
        p->attributes = DiskLe64(e + GPT_ENTRY_ATTR_OFFSET);
        p->first      = first;
        p->last       = last;
        DiskNameOf(e + GPT_ENTRY_NAME_OFFSET, p->name);
    }
    KSort(t->parts, t->part_count, sizeof(DiskPart), DiskPartOrder, NULL);
    return OK;
}

/* One MBR entry `e`, counted from `base`, into the partitions — when it names
 * a run that lies on the disk. */
static void DiskMbrPart(DiskTable* t, const uint8_t* e, uint64_t base, uint16_t entry)
{
    uint64_t start = base + DiskLe32(e + MBR_ENTRY_START);
    uint64_t count = DiskLe32(e + MBR_ENTRY_COUNT_AT);
    if (count == 0) {
        kprintf("[DiskTable] seat %u: MBR entry %u claims no blocks\n", t->seat, entry);
        return;
    }
    if (t->blocks && (start >= t->blocks || count > t->blocks - start)) {
        kprintf("[DiskTable] seat %u: MBR entry %u claims blocks %llu..%llu and the medium has %llu — not "
                "counted\n", t->seat, entry, (unsigned long long)start, (unsigned long long)(start + count - 1),
                (unsigned long long)t->blocks);
        return;
    }
    DiskPart* p = &t->parts[t->part_count++];
    memset(p, 0, sizeof(*p));
    p->entry   = entry;
    p->type[0] = e[MBR_ENTRY_TYPE];
    p->kind    = DiskKindOfMbr(e[MBR_ENTRY_TYPE]);
    p->first   = start;
    p->last    = start + count - 1;
}

static bool DiskMbrExtended(uint8_t type)
{
    return type == 0x05u || type == 0x0Fu || type == 0x85u;
}

/* The partitions of an MBR, the logical ones of its extended entry among
 * them, numbered 4 on in the order of the chain. */
static error_t DiskMbrParts(DiskTable* t, const uint8_t* sector0)
{
    t->parts = (DiskPart*)kmalloc((MBR_ENTRY_COUNT + MBR_LOGICAL_MOST) * sizeof(DiskPart));
    if (!t->parts)
        return ERR_NO_MEMORY;

    uint8_t* ebr = (uint8_t*)kmalloc(t->block_bytes);
    if (!ebr)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        const uint8_t* e    = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        uint8_t        type = e[MBR_ENTRY_TYPE];
        if (type == 0)
            continue;
        if (!DiskMbrExtended(type)) {
            DiskMbrPart(t, e, 0, (uint16_t)i);
            continue;
        }
        uint64_t ext  = DiskLe32(e + MBR_ENTRY_START);
        uint64_t next = 0;
        for (uint32_t n = 0; n < MBR_LOGICAL_MOST; n++) {
            if ((t->blocks && ext + next >= t->blocks) ||
                DiskReadBlocks(t->seat, t->block_bytes, ext + next, 1, ebr) != 0 ||
                ebr[510] != 0x55 || ebr[511] != 0xAA) {
                kprintf("[DiskTable] seat %u: the extended table at block %llu could not be read — the logical "
                        "partitions after it are not counted\n", t->seat, (unsigned long long)(ext + next));
                break;
            }
            const uint8_t* logical = ebr + MBR_TABLE_OFFSET;
            const uint8_t* link    = ebr + MBR_TABLE_OFFSET + MBR_ENTRY_BYTES;
            if (logical[MBR_ENTRY_TYPE] != 0)
                DiskMbrPart(t, logical, ext + next, (uint16_t)(MBR_ENTRY_COUNT + n));
            uint64_t further = DiskLe32(link + MBR_ENTRY_START);
            /* A link that does not go forward is a chain that loops. */
            if (!DiskMbrExtended(link[MBR_ENTRY_TYPE]) || further <= next)
                break;
            next = further;
        }
    }
    kfree(ebr);
    KSort(t->parts, t->part_count, sizeof(DiskPart), DiskPartOrder, NULL);
    return OK;
}

/* The free runs of a GPT disk between first_usable and last_usable: each
 * begins on the grain and holds at least one. */
static error_t DiskFreeRuns(DiskTable* t)
{
    uint64_t grain = DiskTableGrain(t);
    t->free = (DiskFree*)kmalloc((t->part_count + 1u) * sizeof(DiskFree));
    if (!t->free)
        return ERR_NO_MEMORY;
    uint64_t from = t->first_usable;
    for (uint32_t i = 0; i <= t->part_count; i++) {
        uint64_t until = i < t->part_count ? t->parts[i].first : t->last_usable + 1u;
        uint64_t start = (from + grain - 1u) / grain * grain;
        if (until > start && until - start >= grain) {
            t->free[t->free_count].first  = start;
            t->free[t->free_count].blocks = until - start;
            t->free_count++;
        }
        if (i < t->part_count && t->parts[i].last + 1u > from)
            from = t->parts[i].last + 1u;
    }
    return OK;
}

/* ── the table ────────────────────────────────────────────────────────── */

static void DiskSayUnit(const DiskTable* t)
{
    kprintf("[DiskTable] seat %u: its table is written in %u-byte blocks — that is how the medium addresses "
            "itself, this room addresses it in 512-byte sectors, and every number in that table is multiplied by "
            "%u (UEFI 2.10 5.3)\n", t->seat, t->block_bytes, t->block_bytes / BOARDROOM_SECTOR_BYTES);
}

/* The GPT of a disk whose sector 0 says it has one: the first copy, and the
 * far one, and what they say of each other. */
static error_t DiskGptRead(DiskTable* t)
{
    DiskGptCopy    first, far;
    DiskGptCopy*   read = NULL;
    bool           refused = false;
    DiskGptVerdict v = DiskGptCopyRead(t->seat, t->block_bytes, t->blocks, GPT_HEADER_LBA, &first);
    memset(&far, 0, sizeof(far));

    if (v == DISK_GPT_READ) {
        read = &first;
        uint64_t at = t->blocks ? t->blocks - 1u : first.alt_block;
        if (t->blocks && first.alt_block != t->blocks - 1u) {
            /* ‼ The ordinary case of an image written to a larger stick: its
             * far copy sits at the end of the image, not of the medium. Read
             * where the table says, and said once. */
            kprintf("[DiskTable] seat %u: its GPT keeps the far copy at block %llu and the medium runs to %llu — "
                    "this table was made for a smaller disk and copied onto this one\n", t->seat,
                    (unsigned long long)first.alt_block, (unsigned long long)(t->blocks - 1u));
            t->copies |= DISK_COPY_FAR_ELSEWHERE;
            at = first.alt_block;
        }
        if (at <= GPT_HEADER_LBA || (t->blocks && at >= t->blocks) ||
            DiskGptCopyRead(t->seat, t->block_bytes, t->blocks, at, &far) != DISK_GPT_READ) {
            kprintf("[DiskTable] seat %u: the far copy of its GPT does not hold — the first is read\n", t->seat);
            t->copies |= DISK_COPY_FAR_DAMAGED;
        } else if (!DiskGptCopiesAgree(&first, &far)) {
            kprintf("[DiskTable] seat %u: the two copies of its GPT hold and say different things — the first "
                    "is read\n", t->seat);
            t->copies |= DISK_COPY_DIFFER;
        }
    } else {
        refused = v == DISK_GPT_REFUSED;
        /* ‼ A header that is not there is as much damage as one that is wrong:
         * sector 0 said this disk is a GPT. The far copy is in its last block. */
        if (t->blocks) {
            kprintf("[DiskTable] seat %u: its GPT %s — reading the copy at the far end, sector %llu "
                    "(UEFI 2.10 5.3.2)\n", t->seat,
                    v == DISK_GPT_ABSENT ? "is not where the disk says it is"
                    : v == DISK_GPT_REFUSED ? "would not be read" : "is damaged",
                    (unsigned long long)((t->blocks - 1u) * (t->block_bytes / BOARDROOM_SECTOR_BYTES)));
            v = DiskGptCopyRead(t->seat, t->block_bytes, t->blocks, t->blocks - 1u, &far);
            if (v == DISK_GPT_READ) {
                kprintf("[DiskTable] seat %u: the copy at the far end is good — this disk is readable after "
                        "all\n", t->seat);
                t->copies |= DISK_COPY_FIRST_DAMAGED;
                read = &far;
            }
            refused = refused || v == DISK_GPT_REFUSED;
        }
    }

    error_t rc = OK;
    if (read) {
        t->state        = DISK_TABLE_GPT;
        memcpy(t->disk_guid, read->disk_guid, 16);
        t->first_usable = read->first_usable;
        t->last_usable  = read->last_usable;
        t->entry_count  = read->entry_count;
        t->entry_bytes  = read->entry_bytes;
        t->entries      = read->array;
        read->array     = NULL;
        t->first_entries = first.verdict == DISK_GPT_READ ? first.entry_block : 0;
        t->far_header    = far.verdict == DISK_GPT_READ ? far.header_block : 0;
        t->far_entries   = far.verdict == DISK_GPT_READ ? far.entry_block : 0;
        DiskGptCopy view = *read;
        view.array       = t->entries;
        rc = DiskGptParts(t, &view);
        if (rc == OK)
            rc = DiskFreeRuns(t);
        if (t->block_bytes != BOARDROOM_SECTOR_BYTES)
            DiskSayUnit(t);
    } else if (refused) {
        t->state = DISK_TABLE_REFUSED;
        kprintf("[DiskTable] seat %u: the medium refused reads its GPT needs, and no copy was read — what is on "
                "it is not known until it answers\n", t->seat);
    } else {
        t->state = DISK_TABLE_GPT_LOST;
        kprintf("[DiskTable] seat %u: %s\n", t->seat,
                t->blocks ? "neither copy of its GPT could be read — it has no table left to read"
                          : "its GPT is damaged, and the medium will not say how far it runs — so the copy at "
                            "the far end cannot be found");
    }
    DiskGptCopyLetGo(&first);
    DiskGptCopyLetGo(&far);
    return rc;
}

/* The disk's table off the medium, into `t` (its seat, seating and medium
 * already filled). */
static error_t DiskTableReadMedium(DiskTable* t)
{
    uint8_t sector0[BOARDROOM_SECTOR_BYTES];
    if (BoardroomRead(t->seat, 0, 1, sector0) != 0) {
        kprintf("[DiskTable] seat %u: could not be read at all\n", t->seat);
        t->state = DISK_TABLE_REFUSED;
        return OK;
    }
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        kprintf("[DiskTable] seat %u: no partition table on it\n", t->seat);
        t->state = DISK_TABLE_NONE;
        return OK;
    }
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++)
        if (sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES + MBR_ENTRY_TYPE] == DISK_MBR_PROTECTIVE)
            return DiskGptRead(t);

    t->state   = DISK_TABLE_MBR;
    error_t rc = DiskMbrParts(t, sector0);
    if (rc == OK && t->part_count && t->block_bytes != BOARDROOM_SECTOR_BYTES)
        DiskSayUnit(t);
    return rc;
}

/* What the table says, once a read of the medium: how many partitions, and
 * how many of them a BoxOS volume may stand on. */
static void DiskSayRead(const DiskTable* t)
{
    uint32_t boxos = 0;
    for (uint32_t i = 0; i < t->part_count; i++)
        boxos += t->parts[i].kind == DISK_PART_BOXOS;
    if (t->state == DISK_TABLE_GPT)
        kprintf("[DiskTable] seat %u: its GPT names %u partition entr%s, %u partition(s) in them, %u carrying "
                "BoxOS ground, %u free run(s)\n", t->seat, t->entry_count, t->entry_count == 1 ? "y" : "ies",
                t->part_count, boxos, t->free_count);
    else if (t->state == DISK_TABLE_MBR && !boxos)
        kprintf("[DiskTable] seat %u: MBR table read, no BoxOS ground on it — %u partition(s) of other kinds\n",
                t->seat, t->part_count);
}

/* ── what was read, remembered ────────────────────────────────────────────
 *
 * The same disk's table is asked for several times in a boot by parties that
 * have no business knowing about each other: the room describing what it
 * seated, the house asking each chair for its volume, the mount standing on
 * the ground it chose, a person listing the disks. On a flash drive each read
 * is a transfer over the bus, and on a GPT disk four. A table cannot go stale
 * while its medium stays put — nothing but this file's own writer changes it —
 * so it is remembered per chair, keyed by WHICH OCCUPANT of the chair it is
 * of: a stick pulled out and pushed back is a new seating and is read again.
 *
 * A refusal is never remembered: a stick asked in the instant after it was
 * plugged in may refuse and answer the next read.
 *
 * One entry per chair, made when the chair is first read and never given
 * back, as the room keeps its chairs; what an entry holds is replaced.
 * ──────────────────────────────────────────────────────────────────────── */

/* What was read, as one immutable answer: lent to every reader that asks
 * while it is the seat's, let go by the last. */
typedef struct {
    volatile uint32_t refs;
    DiskTable         table;
} DiskSnap;

typedef struct DiskMemo {
    struct DiskMemo* next;
    uint8_t          seat;
    DiskSnap*        snap;      /* NULL: nothing held */
} DiskMemo;

static DiskMemo*         g_memos;
static spinlock_t        g_memo_lock = {0};
/* Odd while a write to some disk's table is under way; every write moves it
 * twice. */
static volatile uint32_t g_writes;

static void DiskTableEmpty(DiskTable* t)
{
    kfree(t->entries);
    kfree(t->parts);
    kfree(t->free);
    t->entries = NULL;
    t->parts   = NULL;
    t->free    = NULL;
}

void DiskTableLetGo(DiskTable* t)
{
    if (t)
        DiskTableEmpty(t);
}

/* A copy of `from` into `to` that owns its own arrays. */
static error_t DiskTableCopy(DiskTable* to, const DiskTable* from)
{
    *to         = *from;
    to->entries = NULL;
    to->parts   = NULL;
    to->free    = NULL;
    size_t entries = (size_t)from->entry_count * from->entry_bytes;
    if ((from->entries && !(to->entries = (uint8_t*)kmalloc(entries))) ||
        (from->part_count && !(to->parts = (DiskPart*)kmalloc(from->part_count * sizeof(DiskPart)))) ||
        (from->free_count && !(to->free = (DiskFree*)kmalloc(from->free_count * sizeof(DiskFree))))) {
        DiskTableEmpty(to);
        return ERR_NO_MEMORY;
    }
    if (from->entries)
        memcpy(to->entries, from->entries, entries);
    if (from->part_count)
        memcpy(to->parts, from->parts, from->part_count * sizeof(DiskPart));
    if (from->free_count)
        memcpy(to->free, from->free, from->free_count * sizeof(DiskFree));
    return OK;
}

static void DiskSnapDrop(DiskSnap* s)
{
    if (s && __atomic_sub_fetch(&s->refs, 1u, __ATOMIC_ACQ_REL) == 0) {
        DiskTableEmpty(&s->table);
        kfree(s);
    }
}

/* The seat's answer copied into `out` — taken under the lock and copied with
 * none held, kmalloc under a spinlock being a wait of unbounded length inside
 * one of bounded length. */
static bool DiskMemoRecall(uint8_t seat, uint32_t seating, DiskTable* out)
{
    DiskSnap* snap = NULL;
    spin_lock(&g_memo_lock);
    for (DiskMemo* m = g_memos; m; m = m->next)
        if (m->seat == seat) {
            if (m->snap && m->snap->table.seating == seating) {
                snap = m->snap;
                __atomic_add_fetch(&snap->refs, 1u, __ATOMIC_ACQ_REL);
            }
            break;
        }
    spin_unlock(&g_memo_lock);
    if (!snap)
        return false;
    bool copied = DiskTableCopy(out, &snap->table) == OK;
    DiskSnapDrop(snap);
    return copied;
}

/* A copy of `t` kept as its seat's answer — unless a write began since
 * `writes` was read: the read may have met half of it. No memory is a table
 * read again next time, which is what every read did before there were
 * memos. */
static void DiskMemoKeep(const DiskTable* t, uint32_t writes)
{
    DiskSnap* snap  = (DiskSnap*)kmalloc(sizeof(DiskSnap));
    DiskMemo* spare = (DiskMemo*)kmalloc(sizeof(DiskMemo));
    if (!snap || DiskTableCopy(&snap->table, t) != OK) {
        kfree(snap);
        kfree(spare);
        return;
    }
    snap->refs = 1;

    DiskSnap* old = snap;
    spin_lock(&g_memo_lock);
    if ((writes & 1u) == 0 && __atomic_load_n(&g_writes, __ATOMIC_ACQUIRE) == writes) {
        DiskMemo* m = NULL;
        for (DiskMemo* c = g_memos; c; c = c->next)
            if (c->seat == t->seat) {
                m = c;
                break;
            }
        if (!m && spare) {
            m       = spare;
            spare   = NULL;
            m->seat = t->seat;
            m->snap = NULL;
            m->next = g_memos;
            g_memos = m;
        }
        if (m) {
            old     = m->snap;
            m->snap = snap;
        }
    }
    spin_unlock(&g_memo_lock);
    DiskSnapDrop(old);
    kfree(spare);
}

void DiskTableForget(uint8_t seat)
{
    DiskSnap* old = NULL;
    spin_lock(&g_memo_lock);
    for (DiskMemo* m = g_memos; m; m = m->next)
        if (m->seat == seat) {
            old     = m->snap;
            m->snap = NULL;
            break;
        }
    spin_unlock(&g_memo_lock);
    DiskSnapDrop(old);
}

void DiskWriteBegins(void)
{
    __atomic_add_fetch(&g_writes, 1u, __ATOMIC_ACQ_REL);
}

void DiskWriteEnds(uint8_t seat)
{
    DiskTableForget(seat);
    __atomic_add_fetch(&g_writes, 1u, __ATOMIC_ACQ_REL);
}

error_t DiskTableRead(uint8_t seat, DiskTable* out)
{
    memset(out, 0, sizeof(*out));
    uint32_t seating = BoardroomSeatSeating(seat);
    if (seating == 0 || !BoardroomSeatOccupied(seat))
        return ERR_OBJECT_NOT_FOUND;
    if (DiskMemoRecall(seat, seating, out))
        return OK;

    uint32_t writes     = __atomic_load_n(&g_writes, __ATOMIC_ACQUIRE);
    out->seat           = seat;
    out->seating        = seating;
    out->block_bytes    = BoardroomSeatLogicalBytes(seat);
    out->physical_bytes = BoardroomSeatPhysicalBytes(seat);
    if (out->block_bytes < BOARDROOM_SECTOR_BYTES) {
        kprintf("[DiskTable] seat %u: the medium says no block a table could be read in — not reading it\n", seat);
        out->state = DISK_TABLE_NONE;
        return OK;
    }
    out->blocks = BoardroomSeatSectors(seat) / (out->block_bytes / BOARDROOM_SECTOR_BYTES);
    if (out->blocks && out->blocks < 2u * GPT_HEADER_LBA + 1u) {
        kprintf("[DiskTable] seat %u: the medium runs to %llu block(s), too few for a table — not reading it\n",
                seat, (unsigned long long)out->blocks);
        out->state = DISK_TABLE_NONE;
        return OK;
    }

    error_t rc = DiskTableReadMedium(out);
    if (rc != OK) {
        DiskTableEmpty(out);
        return rc;
    }
    DiskSayRead(out);
    if (out->state != DISK_TABLE_REFUSED)
        DiskMemoKeep(out, writes);
    return OK;
}
