#include "cow.h"
#include "../mount/ground_census.h"
#include "../disk_book/disk_book.h"
#include "../tagfs.h"

// ----------------------------------------------------------------------------
// The snapshots an earlier kernel left, read at mount
// ----------------------------------------------------------------------------

/* DiskBookRedirectSeen: one redirect, to the snapshot it names. Only the block
 * the snapshot kept is taken down; the block its file moved onto is that
 * file's, named by its own record. */
static void CowRedirectSeen(uint32_t snapshot_id, uint32_t old_block,
                            uint32_t new_block, void *ctx)
{
    CowReading  *reading = ctx;
    CowSnapshot *snap    = NULL;

    if (old_block == 0 || new_block == 0)
        return;
    for (uint32_t i = 0; i < reading->count && !snap; i++)
        if (reading->snapshots[i].snapshot_id == snapshot_id)
            snap = &reading->snapshots[i];
    if (!snap) {
        reading->stray++;
        return;
    }

    for (uint32_t k = 0; k < snap->kept_count; k++)
        if (snap->kept[k] == old_block)
            return;

    if (snap->kept_count == snap->kept_cap) {
        uint32_t  cap   = snap->kept_cap ? snap->kept_cap * 2u : 16u;
        uint32_t *grown = kmalloc(sizeof(uint32_t) * cap);
        if (!grown) {
            reading->short_of_memory = true;
            return;
        }
        if (snap->kept_count)
            memcpy(grown, snap->kept, sizeof(uint32_t) * snap->kept_count);
        kfree(snap->kept);
        snap->kept     = grown;
        snap->kept_cap = cap;
    }
    snap->kept[snap->kept_count++] = old_block;
}

/* The manifest at `block`, read into a list of its own: NULL with *count 0
 * for a manifest that holds none, or one this mount could not read. */
static CowSnapshot *CowManifestRead(uint32_t block, uint32_t *count)
{
    *count = 0;
    CowManifest *manifest = kmalloc(sizeof(CowManifest));
    if (!manifest) {
        kprintf("[CoW] no memory to read the snapshot manifest at data block %u — the "
                "snapshots an earlier kernel left are not named\n", block);
        return NULL;
    }
    if (tagfs_read_block(block, manifest) != OK ||
        manifest->magic != COW_MANIFEST_MAGIC || manifest->count > COW_MANIFEST_MAX) {
        kprintf("[CoW] the snapshot manifest the Ledger names at data block %u does not "
                "read as one — the snapshots an earlier kernel left are not named\n", block);
        kfree(manifest);
        return NULL;
    }

    uint32_t     n         = manifest->count;
    CowSnapshot *snapshots = n ? kmalloc(sizeof(CowSnapshot) * n) : NULL;
    if (n && !snapshots) {
        kprintf("[CoW] no memory to hold the %u snapshot(s) the manifest at data block %u "
                "names — they are not named\n", n, block);
        kfree(manifest);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        const CowSnapshotDisk *from = &manifest->entries[i];
        CowSnapshot           *to   = &snapshots[i];
        memset(to, 0, sizeof(*to));
        to->snapshot_id    = from->snapshot_id;
        to->parent_file_id = from->parent_file_id;
        memcpy(to->name, from->name, sizeof(from->name));
    }
    kfree(manifest);
    *count = n;
    return snapshots;
}

void CowReadStanding(uint32_t manifest_block, uint32_t book_block, CowReading *out)
{
    memset(out, 0, sizeof(*out));
    if (manifest_block == 0)
        return;
    out->snapshots = CowManifestRead(manifest_block, &out->count);
    if (out->count)
        out->book = DiskBookReplay(book_block, CowRedirectSeen, out);
}

/* Said so a volume that carries them names which, and that what they kept is
 * the census's to name — not lost, and not a fault of this kernel's. */
void CowSayStanding(const CowReading *reading)
{
    kprintf("[CoW] this volume carries %u snapshot(s) an earlier kernel took — snapshots "
            "arrive with format v2, and nothing reads these, so the blocks they keep are "
            "owned by nothing: the census names them among its orphans and keeps them "
            "taken until it gives orphans back\n", reading->count);
    for (uint32_t i = 0; i < reading->count; i++) {
        const CowSnapshot *snap = &reading->snapshots[i];
        if (snap->parent_file_id)
            kprintf("[CoW]   '%s' (id %u) of file %u keeps %u block(s)\n", snap->name,
                    snap->snapshot_id, snap->parent_file_id, snap->kept_count);
        else
            kprintf("[CoW]   '%s' (id %u) of the whole volume keeps %u block(s)\n",
                    snap->name, snap->snapshot_id, snap->kept_count);
    }
    if (reading->stray)
        kprintf("[CoW] %u redirect(s) in the DiskBook name no snapshot of the manifest, "
                "and are passed over\n", reading->stray);
    if (reading->book == ERR_NO_MEMORY)
        kprintf("[CoW] no memory to replay the DiskBook — the blocks the snapshots keep "
                "are not counted here\n");
    else if (reading->book != OK)
        kprintf("[CoW] the DiskBook's head is not one this kernel takes — no redirect is "
                "replayed, and the blocks the snapshots keep are not counted here\n");
    if (reading->short_of_memory)
        kprintf("[CoW] memory ran out while the DiskBook was replayed — some blocks the "
                "snapshots keep are not counted here\n");
}

void CowReadingDrop(CowReading *reading)
{
    for (uint32_t i = 0; i < reading->count; i++)
        kfree(reading->snapshots[i].kept);
    kfree(reading->snapshots);
    memset(reading, 0, sizeof(*reading));
}

void CowNameStanding(void)
{
    TagFSState *fs = tagfs_get_state();
    CowReading  reading;

    CowReadStanding(fs->ledger.cow_manifest_block, TagFSLayout()->disk_book_block, &reading);
    if (reading.count)
        CowSayStanding(&reading);
    CowReadingDrop(&reading);
}

/*
 * ‼ THE MANIFEST IS THE ONE BLOCK COPY-ON-WRITE OWNS.
 *
 * The mount's census asks every owner of the data run for its ground
 * (ground_owner.h). The manifest is named by the Ledger and read by every
 * mount, so it is owned while the Ledger names it. What the snapshots kept is
 * not claimed: nothing reads a v1 snapshot, so those blocks are orphans the
 * census names and keeps taken. Nor is the block a redirect's file moved onto:
 * that file's record claims it, and once the file was deleted a claim here
 * took the freed block again as missing on every mount after.
 *
 * The step that lets the census give orphans back retires the manifest with
 * it: a kernel from before format v1's refusal would otherwise replay these
 * redirects over blocks other files hold by then, and free them with its
 * snapshot.
 */
void CowClaimGround(uint32_t manifest_block)
{
    if (manifest_block)
        GroundClaim(GROUND_MANIFEST, manifest_block, 1, 0);
}
