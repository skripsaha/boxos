#include "disk_book.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"

/*
 * The book's three parts are whole blocks apart, from the block the Deed
 * names: its head, the copy of its head, and the run its records live in —
 * so no record shares a block of the medium with its only copy. Every sector
 * is counted from the start of the volume and read through the volume's own
 * door (tagfs_volume_read), never the Boardroom's: going to the medium itself
 * would mean knowing where the volume begins, which is exactly the knowledge
 * that stopped a volume from being able to move.
 */
static uint64_t DiskBookPartAt(uint32_t book_block, uint32_t part)
{
    return ((uint64_t)book_block + part) * TAGFS_BLOCK_SECTORS;
}

/* Whether a piece is whole: its magic, and a CRC32 over it with its own
 * checksum field taken as zero — asked in place, so nothing is copied onto
 * the stack to be checked. */
static bool DiskBookHeadWhole(DiskBookSuperblock *h)
{
    uint32_t stored = h->crc32;
    h->crc32 = 0;
    bool whole = h->magic == DISK_BOOK_SB_MAGIC &&
                 KCrc32((const uint8_t *)h, sizeof(*h)) == stored;
    h->crc32 = stored;
    return whole;
}

static bool DiskBookRecordWhole(DiskBookEntry *e)
{
    uint32_t stored = e->record_crc32;
    e->record_crc32 = 0;
    bool whole = e->magic == DISK_BOOK_MAGIC &&
                 KCrc32((const uint8_t *)e, sizeof(*e)) == stored;
    e->record_crc32 = stored;
    return whole;
}

/* The copy of the head at `at`, read into `h` and taken only when it is whole
 * and describes the book this kernel knows: its version, its geometry, and
 * its records where the Deed puts them. */
static bool DiskBookHeadTaken(uint64_t at, uint64_t records_at, DiskBookSuperblock *h)
{
    return tagfs_volume_read(at, 1, h) == 0 && DiskBookHeadWhole(h) &&
           h->version == DISK_BOOK_VERSION && h->capacity == DISK_BOOK_CAPACITY &&
           h->count <= DISK_BOOK_CAPACITY && h->start_sector == records_at;
}

error_t DiskBookReplay(uint32_t book_block, DiskBookRedirectSeen seen, void *ctx)
{
    uint64_t records_at = DiskBookPartAt(book_block, 2);

    /* One record's room, the head read into it first. */
    DiskBookEntry *room = kmalloc(sizeof(DiskBookEntry));
    if (!room)
        return ERR_NO_MEMORY;
    DiskBookSuperblock *head = (DiskBookSuperblock *)room;
    if (!DiskBookHeadTaken(DiskBookPartAt(book_block, 0), records_at, head) &&
        !DiskBookHeadTaken(DiskBookPartAt(book_block, 1), records_at, head)) {
        kfree(room);
        return ERR_TAGFS_CORRUPTED;
    }

    uint32_t count   = head->count;
    uint32_t read    = 0;
    uint32_t skipped = 0;
    for (; read < count; read++) {
        uint64_t at = records_at + (uint64_t)read * DISK_BOOK_SECTORS_PER_ENTRY;
        if (tagfs_volume_read(at, DISK_BOOK_SECTORS_PER_ENTRY, room) != 0)
            break;
        if (!DiskBookRecordWhole(room)) {
            skipped++;
            continue;
        }
        if (room->type == DISK_BOOK_TYPE_REDIRECT)
            seen(room->snapshot_id, room->old_block, room->new_block, ctx);
    }
    kfree(room);

    if (read < count)
        kprintf("[DiskBook] record %u of %u would not read — the redirects from it on "
                "are not replayed\n", read, count);
    if (skipped)
        kprintf("[DiskBook] %u of %u redirect record(s) fail their magic or checksum and "
                "are skipped\n", skipped, read);
    return OK;
}
