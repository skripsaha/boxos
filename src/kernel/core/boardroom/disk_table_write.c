#include "disk_table.h"
#include "disk_table_internal.h"
#include "boardroom.h"
#include "random.h"
#include "klib.h"
#include "crypto.h"

/*
 * The writing of a disk's table (disk_table.h) — the one place in the kernel
 * that writes one.
 *
 * ‼ A CUT LEAVES A WHOLE TABLE, THE OLD ONE OR THE NEW. A GPT is kept twice,
 * the first copy at block 1 and the far one at the end, and a reader believes
 * the first that holds (disk_table.c). So the far copy is written first and
 * flushed, then the first copy and flushed: cut before the far copy is whole,
 * the first is the old table and still holds; cut between, the two copies
 * hold and differ, and the first — the old — is read; cut inside the first,
 * its checksums fail and the far copy — the new — is read. A whole new table
 * over a disk the old MBR way laid writes its protective MBR last of all: until
 * that sector lands the disk is the old MBR one, whole. Every write is read
 * back and held to what was meant before the change is answered done.
 *
 * ‼ NOTHING A RUNNING SYSTEM STANDS ON. A disk that carries a volume of the
 * house is never laid anew, and a partition one stands on is never taken
 * away; the asker names what stands where (DiskInUse), the room knowing
 * media and not volumes. Only BoxOS's own partitions are taken away, and
 * only free runs given.
 *
 * ‼ THE DISK A PERSON LOOKED AT. Every change names the seat and the seating
 * the person saw: a seat that changed hands since is refused, and is asked
 * again before each write goes out.
 *
 * One change at a time, of any disk: a second asked meanwhile is refused
 * (ERR_WOULD_BLOCK) — changes of a table are a person's, and rare.
 */

static volatile uint32_t g_changing;

/* The table a change writes: what both copies say. */
typedef struct {
    uint8_t  disk_guid[16];
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t entry_count;
    uint32_t entry_bytes;
    uint64_t first_entries;   /* the first copy's array */
    uint64_t far_header;
    uint64_t far_entries;
    uint8_t* entries;         /* entry_count × entry_bytes, owned */
} DiskPlan;

static void DiskPutLe32(uint8_t* p, uint32_t v)
{
    for (uint32_t i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}

static void DiskPutLe64(uint8_t* p, uint64_t v)
{
    for (uint32_t i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}

/* The bytes of one GPT header into a block of its own. */
static void DiskHeaderLay(uint8_t* block, uint32_t block_bytes, const DiskPlan* p, uint64_t my, uint64_t alt,
                          uint64_t entries, uint32_t array_crc)
{
    memset(block, 0, block_bytes);
    memcpy(block + GPT_SIG_OFFSET, "EFI PART", 8);
    DiskPutLe32(block + GPT_REVISION_OFFSET, GPT_REVISION);
    DiskPutLe32(block + GPT_HEADER_SIZE_OFFSET, GPT_HEADER_MIN_BYTES);
    DiskPutLe64(block + GPT_MY_LBA_OFFSET, my);
    DiskPutLe64(block + GPT_ALT_LBA_OFFSET, alt);
    DiskPutLe64(block + GPT_FIRST_USABLE_OFFSET, p->first_usable);
    DiskPutLe64(block + GPT_LAST_USABLE_OFFSET, p->last_usable);
    memcpy(block + GPT_DISK_GUID_OFFSET, p->disk_guid, 16);
    DiskPutLe64(block + GPT_ENTRY_LBA_OFFSET, entries);
    DiskPutLe32(block + GPT_ENTRY_COUNT_OFFSET, p->entry_count);
    DiskPutLe32(block + GPT_ENTRY_BYTES_OFFSET, p->entry_bytes);
    DiskPutLe32(block + GPT_ENTRY_CRC_OFFSET, array_crc);
    DiskPutLe32(block + GPT_HEADER_CRC_OFFSET, KCrc32(block, GPT_HEADER_MIN_BYTES));
}

/* Whether the seat still holds the disk the change was asked of. */
static bool DiskStillThere(const DiskTable* t)
{
    return BoardroomSeatOccupied(t->seat) && BoardroomSeatSeating(t->seat) == t->seating;
}

/* What a refused write of a table answers. */
static error_t DiskRefusal(const DiskTable* t, const char* what)
{
    if (!DiskStillThere(t)) {
        kprintf("[DiskTable] seat %u: the disk left while %s was written — its table is the old one or the new, "
                "whole, as the next read says\n", t->seat, what);
        return ERR_DEVICE_NOT_READY;
    }
    if (BoardroomSeatRefusesWrites(t->seat)) {
        kprintf("[DiskTable] seat %u: the medium refuses writes — it is protected, and its table is as it was\n",
                t->seat);
        return ERR_PERMISSION_DENIED;
    }
    kprintf("[DiskTable] seat %u: the medium refused the write of %s — its table is the old one or the new, whole, "
            "as the next read says\n", t->seat, what);
    return ERR_WRITE_FAILED;
}

/* The medium's blocks [block, block + count) from `buf`, the seat asked first. */
static error_t DiskWriteBlocks(const DiskTable* t, uint64_t block, uint64_t count, const void* buf, const char* what)
{
    uint64_t ratio = t->block_bytes / BOARDROOM_SECTOR_BYTES;
    if (!DiskStillThere(t) || BoardroomWrite(t->seat, block * ratio, (uint32_t)(count * ratio), buf, NULL) != 0)
        return DiskRefusal(t, what);
    return OK;
}

static error_t DiskFlush(const DiskTable* t, const char* what)
{
    if (!DiskStillThere(t) || BoardroomFlush(t->seat) != 0)
        return DiskRefusal(t, what);
    return OK;
}

/* One copy of the plan: its array, then its header. */
static error_t DiskCopyWrite(const DiskTable* t, const DiskPlan* p, uint8_t* header, uint64_t my, uint64_t alt,
                             uint64_t entries, uint32_t array_crc, uint64_t array_blocks, const char* what)
{
    error_t rc = DiskWriteBlocks(t, entries, array_blocks, p->entries, what);
    if (rc != OK)
        return rc;
    DiskHeaderLay(header, t->block_bytes, p, my, alt, entries, array_crc);
    return DiskWriteBlocks(t, my, 1, header, what);
}

/* The copy read back at `my`, held to the plan. */
static bool DiskCopyHolds(const DiskTable* t, const DiskPlan* p, uint64_t my, uint32_t array_crc)
{
    DiskGptCopy c;
    bool holds = DiskGptCopyRead(t->seat, t->block_bytes, t->blocks, my, &c) == DISK_GPT_READ &&
                 c.array_crc == array_crc && c.first_usable == p->first_usable &&
                 c.last_usable == p->last_usable && memcmp(c.disk_guid, p->disk_guid, 16) == 0 &&
                 c.entry_count == p->entry_count && c.entry_bytes == p->entry_bytes;
    DiskGptCopyLetGo(&c);
    return holds;
}

/* The sector that says the disk is a GPT (UEFI 2.10 §5.2.3): one entry of
 * type 0xEE from block 1 to the end — as far as 32 bits count — and no boot
 * code, laid at the head of block 0. */
static void DiskProtectiveLay(uint8_t* block, const DiskTable* t)
{
    memset(block, 0, t->block_bytes);
    uint8_t* e     = block + 446;
    uint64_t count = t->blocks - 1u;
    e[1] = 0x00;
    e[2] = 0x02;
    e[3] = 0x00;
    e[4] = DISK_MBR_PROTECTIVE;
    e[5] = 0xFF;
    e[6] = 0xFF;
    e[7] = 0xFF;
    DiskPutLe32(e + 8, 1u);
    DiskPutLe32(e + 12, count > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)count);
    block[510] = 0x55;
    block[511] = 0xAA;
}

/*
 * The plan written over `t`'s disk: the far copy and a flush, the first copy
 * and a flush, and — when `protective` — the protective MBR and a flush; each
 * copy read back after. The far copy at the last block, its array before it.
 */
static error_t DiskPlanWrite(const DiskTable* t, const DiskPlan* p, bool protective, const char* what)
{
    uint32_t array_bytes  = p->entry_count * p->entry_bytes;
    uint64_t array_blocks = (array_bytes + t->block_bytes - 1u) / t->block_bytes;
    uint32_t array_crc    = KCrc32(p->entries, array_bytes);
    uint8_t* header       = (uint8_t*)kmalloc(t->block_bytes);
    if (!header)
        return ERR_NO_MEMORY;

    DiskWriteBegins();
    error_t rc = DiskCopyWrite(t, p, header, p->far_header, GPT_HEADER_LBA, p->far_entries, array_crc, array_blocks,
                               what);
    if (rc == OK)
        rc = DiskFlush(t, what);
    if (rc == OK)
        rc = DiskCopyWrite(t, p, header, GPT_HEADER_LBA, p->far_header, p->first_entries, array_crc, array_blocks,
                           what);
    if (rc == OK)
        rc = DiskFlush(t, what);
    if (rc == OK && protective) {
        DiskProtectiveLay(header, t);
        rc = DiskWriteBlocks(t, 0, 1, header, what);
        if (rc == OK)
            rc = DiskFlush(t, what);
    }
    if (rc == OK && (!DiskCopyHolds(t, p, GPT_HEADER_LBA, array_crc) || !DiskCopyHolds(t, p, p->far_header, array_crc))) {
        kprintf("[DiskTable] seat %u: %s was written and does not read back as it was meant\n", t->seat, what);
        rc = ERR_WRITE_FAILED;
    }
    DiskWriteEnds(t->seat);
    kfree(header);
    return rc;
}

/* A plan of `t`'s own table, to be changed: its geometry kept, its array
 * copied. */
static error_t DiskPlanOf(const DiskTable* t, DiskPlan* p)
{
    memcpy(p->disk_guid, t->disk_guid, 16);
    p->first_usable  = t->first_usable;
    p->last_usable   = t->last_usable;
    p->entry_count   = t->entry_count;
    p->entry_bytes   = t->entry_bytes;
    p->first_entries = t->first_entries;
    p->far_header    = t->far_header;
    p->far_entries   = t->far_entries;
    size_t bytes     = (size_t)t->entry_count * t->entry_bytes;
    p->entries       = (uint8_t*)kmalloc(bytes);
    if (!p->entries)
        return ERR_NO_MEMORY;
    memcpy(p->entries, t->entries, bytes);
    return OK;
}

/* The table of the disk asked for, read afresh, and the one change of the
 * machine at a time taken: OK, or the refusal, nothing taken. */
static error_t DiskChangeBegin(uint8_t seat, uint32_t seating, DiskTable* t)
{
    if (__atomic_exchange_n(&g_changing, 1u, __ATOMIC_ACQ_REL))
        return ERR_WOULD_BLOCK;
    DiskTableForget(seat);
    error_t rc = DiskTableRead(seat, t);
    if (rc == OK && t->seating != seating) {
        DiskTableLetGo(t);
        rc = ERR_OBJECT_NOT_FOUND;
    }
    if (rc == OK && t->state == DISK_TABLE_REFUSED) {
        DiskTableLetGo(t);
        rc = ERR_DEVICE_NOT_READY;
    }
    if (rc != OK)
        __atomic_store_n(&g_changing, 0u, __ATOMIC_RELEASE);
    return rc;
}

static void DiskChangeEnd(DiskTable* t)
{
    DiskTableLetGo(t);
    __atomic_store_n(&g_changing, 0u, __ATOMIC_RELEASE);
}

/* A table one may change a partition of: a GPT whose copies hold, agree and
 * lie where they should. */
static error_t DiskChangeable(const DiskTable* t)
{
    if (t->state != DISK_TABLE_GPT)
        return ERR_INVALID_STATE;
    if (t->copies)
        return ERR_CORRUPTED;
    return OK;
}

static bool DiskPartUsed(const DiskInUse* used, uint64_t first)
{
    for (uint32_t i = 0; used && i < used->count; i++)
        if (used->firsts[i] == first)
            return true;
    return false;
}

error_t DiskTableLay(uint8_t seat, uint32_t seating, const DiskInUse* used)
{
    DiskTable t;
    error_t   rc = DiskChangeBegin(seat, seating, &t);
    if (rc != OK)
        return rc;
    if (used && used->count) {
        kprintf("[DiskTable] seat %u: a volume of the house stands on this disk — it is not laid anew\n", seat);
        DiskChangeEnd(&t);
        return ERR_BUSY;
    }
    if (!t.blocks) {
        kprintf("[DiskTable] seat %u: the medium will not say how far it runs — no table can be laid on it\n", seat);
        DiskChangeEnd(&t);
        return ERR_INVALID_STATE;
    }

    /* The least the specification reserves for the array, and the first and
     * last usable blocks around the two copies (UEFI 2.10 §5.3.2). */
    uint64_t array_blocks = (GPT_WRITE_ENTRIES * GPT_WRITE_ENTRY_BYTES + t.block_bytes - 1u) / t.block_bytes;
    uint64_t grain        = DiskTableGrain(&t);
    if (t.blocks < 2u * grain + 2u * (array_blocks + 1u)) {
        kprintf("[DiskTable] seat %u: %llu block(s) is too small a disk to hold a table and a partition\n", seat,
                (unsigned long long)t.blocks);
        DiskChangeEnd(&t);
        return ERR_OUT_OF_RANGE;
    }
    DiskPlan p;
    memset(&p, 0, sizeof(p));
    RandomGuid(p.disk_guid);
    p.entry_count   = GPT_WRITE_ENTRIES;
    p.entry_bytes   = GPT_WRITE_ENTRY_BYTES;
    p.first_entries = GPT_HEADER_LBA + 1u;
    p.first_usable  = p.first_entries + array_blocks;
    p.far_header    = t.blocks - 1u;
    p.far_entries   = p.far_header - array_blocks;
    p.last_usable   = p.far_entries - 1u;
    p.entries       = (uint8_t*)kmalloc(GPT_WRITE_ENTRIES * GPT_WRITE_ENTRY_BYTES);
    if (!p.entries) {
        DiskChangeEnd(&t);
        return ERR_NO_MEMORY;
    }
    memset(p.entries, 0, GPT_WRITE_ENTRIES * GPT_WRITE_ENTRY_BYTES);

    kprintf("[DiskTable] seat %u: a new GPT is laid over the whole disk — %u partition(s) it named are gone\n",
            seat, t.part_count);
    rc = DiskPlanWrite(&t, &p, true, "a new table");
    if (rc == OK)
        kprintf("[DiskTable] seat %u: laid — a GPT of %u entries, blocks %llu..%llu free\n", seat, p.entry_count,
                (unsigned long long)p.first_usable, (unsigned long long)p.last_usable);
    kfree(p.entries);
    DiskChangeEnd(&t);
    return rc;
}

/* A GPT name — UTF-8 in, at most 36 UTF-16LE units out, the rest zeros. */
static void DiskNameLay(uint8_t* units, const char* name)
{
    memset(units, 0, GPT_ENTRY_NAME_UNITS * 2u);
    for (uint32_t i = 0; name[i] && i < GPT_ENTRY_NAME_UNITS; i++) {
        units[2u * i]      = (uint8_t)name[i];
        units[2u * i + 1u] = 0;
    }
}

error_t DiskTableAdd(uint8_t seat, uint32_t seating, DiskPartKind kind, uint64_t first, uint64_t blocks,
                     uint16_t* entry)
{
    static const uint8_t esp[16]   = DISK_TYPE_ESP;
    static const uint8_t boxos[16] = DISK_TYPE_BOXOS;
    static const uint8_t unused[16] = { 0 };
    if (kind != DISK_PART_ESP && kind != DISK_PART_BOXOS)
        return ERR_INVALID_ARGUMENT;

    DiskTable t;
    error_t   rc = DiskChangeBegin(seat, seating, &t);
    if (rc != OK)
        return rc;
    rc = DiskChangeable(&t);

    /* Inside one free run, from its grain. */
    uint64_t grain = DiskTableGrain(&t);
    bool     fits  = false;
    for (uint32_t i = 0; rc == OK && i < t.free_count; i++)
        fits = fits || (first >= t.free[i].first && first - t.free[i].first < t.free[i].blocks &&
                        first % grain == 0 && blocks >= grain &&
                        blocks <= t.free[i].blocks - (first - t.free[i].first));
    if (rc == OK && !fits) {
        kprintf("[DiskTable] seat %u: blocks %llu..+%llu are not a run on the grain inside one free run — nothing "
                "laid\n", seat, (unsigned long long)first, (unsigned long long)blocks);
        rc = ERR_OUT_OF_RANGE;
    }

    DiskPlan p;
    memset(&p, 0, sizeof(p));
    if (rc == OK)
        rc = DiskPlanOf(&t, &p);
    uint32_t slot = 0;
    while (rc == OK && slot < p.entry_count && memcmp(p.entries + (uint64_t)slot * p.entry_bytes, unused, 16) != 0)
        slot++;
    if (rc == OK && slot == p.entry_count) {
        kprintf("[DiskTable] seat %u: its table has no free entry left of %u\n", seat, p.entry_count);
        rc = ERR_QUOTA_EXCEEDED;
    }
    if (rc == OK) {
        uint8_t* e = p.entries + (uint64_t)slot * p.entry_bytes;
        memset(e, 0, p.entry_bytes);
        memcpy(e + GPT_ENTRY_TYPE_OFFSET, kind == DISK_PART_ESP ? esp : boxos, 16);
        RandomGuid(e + GPT_ENTRY_UNIQUE_OFFSET);
        DiskPutLe64(e + GPT_ENTRY_FIRST_OFFSET, first);
        DiskPutLe64(e + GPT_ENTRY_LAST_OFFSET, first + blocks - 1u);
        DiskNameLay(e + GPT_ENTRY_NAME_OFFSET, kind == DISK_PART_ESP ? "EFI system partition" : "BoxOS");
        rc = DiskPlanWrite(&t, &p, false, "a partition");
        if (rc == OK) {
            kprintf("[DiskTable] seat %u: %s %s partition laid in entry %u, blocks %llu..%llu\n", seat,
                    kind == DISK_PART_ESP ? "an" : "a",
                    DiskKindName(kind), slot, (unsigned long long)first, (unsigned long long)(first + blocks - 1u));
            if (entry)
                *entry = (uint16_t)slot;
        }
    }
    kfree(p.entries);
    DiskChangeEnd(&t);
    return rc;
}

error_t DiskTableRemove(uint8_t seat, uint32_t seating, uint16_t entry, const DiskInUse* used)
{
    DiskTable t;
    error_t   rc = DiskChangeBegin(seat, seating, &t);
    if (rc != OK)
        return rc;
    rc = DiskChangeable(&t);

    const DiskPart* part = NULL;
    for (uint32_t i = 0; rc == OK && i < t.part_count; i++)
        if (t.parts[i].entry == entry)
            part = &t.parts[i];
    if (rc == OK && !part)
        rc = ERR_OUT_OF_RANGE;
    else if (rc == OK && part->kind != DISK_PART_BOXOS) {
        kprintf("[DiskTable] seat %u: entry %u is a %s partition — only BoxOS's own are taken away\n", seat, entry,
                DiskKindName(part->kind));
        rc = ERR_INVALID_ARGUMENT;
    } else if (rc == OK && DiskPartUsed(used, part->first)) {
        kprintf("[DiskTable] seat %u: a volume of the house stands on entry %u — it is not taken away\n", seat,
                entry);
        rc = ERR_BUSY;
    }

    DiskPlan p;
    memset(&p, 0, sizeof(p));
    if (rc == OK)
        rc = DiskPlanOf(&t, &p);
    if (rc == OK) {
        memset(p.entries + (uint64_t)entry * p.entry_bytes, 0, p.entry_bytes);
        rc = DiskPlanWrite(&t, &p, false, "a partition taken away");
        if (rc == OK)
            kprintf("[DiskTable] seat %u: entry %u taken away — blocks %llu..%llu are free\n", seat, entry,
                    (unsigned long long)part->first, (unsigned long long)part->last);
    }
    kfree(p.entries);
    DiskChangeEnd(&t);
    return rc;
}

error_t DiskTableMend(uint8_t seat, uint32_t seating)
{
    DiskTable t;
    error_t   rc = DiskChangeBegin(seat, seating, &t);
    if (rc != OK)
        return rc;
    if (t.state != DISK_TABLE_GPT) {
        DiskChangeEnd(&t);
        return ERR_INVALID_STATE;
    }
    if (!t.copies) {
        DiskChangeEnd(&t);
        return OK;
    }
    if (!t.blocks) {
        kprintf("[DiskTable] seat %u: the medium will not say how far it runs — its far copy cannot be put back\n",
                seat);
        DiskChangeEnd(&t);
        return ERR_INVALID_STATE;
    }

    /* Both copies from the one read, the far one at the end of the disk. A
     * table made for a smaller disk is given the blocks it never knew of:
     * every partition lies below its old last usable block, so none moves. */
    DiskPlan p;
    memset(&p, 0, sizeof(p));
    rc = DiskPlanOf(&t, &p);
    if (rc == OK) {
        uint64_t array_blocks = ((uint64_t)p.entry_count * p.entry_bytes + t.block_bytes - 1u) / t.block_bytes;
        p.far_header  = t.blocks - 1u;
        p.far_entries = p.far_header - array_blocks;
        if (t.copies & DISK_COPY_FAR_ELSEWHERE)
            p.last_usable = p.far_entries - 1u;
        if (t.copies & DISK_COPY_FIRST_DAMAGED)
            p.first_entries = GPT_HEADER_LBA + 1u;
        if (p.first_entries + array_blocks > p.first_usable || p.far_entries <= p.last_usable) {
            kprintf("[DiskTable] seat %u: its table's copies would not fit around the blocks it may use — it is "
                    "not mended, and a new table is the way left\n", seat);
            rc = ERR_INVALID_STATE;
        }
    }
    if (rc == OK) {
        rc = DiskPlanWrite(&t, &p, false, "the mended table");
        if (rc == OK)
            kprintf("[DiskTable] seat %u: its table is mended — both copies hold and agree%s\n", seat,
                    (t.copies & DISK_COPY_FAR_ELSEWHERE) ? ", the far one at the end of the disk" : "");
    }
    kfree(p.entries);
    DiskChangeEnd(&t);
    return rc;
}
