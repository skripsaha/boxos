#include "cow.h"
#include "../disk_book/disk_book.h"
#include "../tagfs.h"

/* A block a snapshot kept (old) and the one its file moved onto (new). */
typedef struct {
    uint32_t old_block;
    uint32_t new_block;
} CowRedirect;

/* One snapshot an earlier kernel left: what its manifest entry says of it,
 * and the redirects the DiskBook replays into it. */
typedef struct {
    uint32_t     snapshot_id;
    uint32_t     parent_file_id;               /* 0: the whole volume */
    char         name[sizeof(((CowSnapshotDisk *)0)->name) + 1];
    CowRedirect *redirects;
    uint32_t     redirect_count;
    uint32_t     redirect_cap;
} CowSnapshot;

/* The snapshots standing on the mounted volume. Built by the mount off to the
 * side and published whole under the lock, never changed after that, and
 * taken down whole: so under the lock there is only ever a pointer to take or
 * to read. */
typedef struct {
    CowSnapshot *snapshots;
    uint32_t     snapshot_count;
    TagFSLock    lock;
    bool         initialized;
} CowState;

static CowState g_cow_state = { .lock = TAGFS_LOCK_INIT("cow", LOCK_RANK_COW) };

/* Said once a boot: the refusal itself is every caller's, on every request. */
static bool g_refusal_said;

static void CowSnapshotsFree(CowSnapshot *snapshots, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        kfree(snapshots[i].redirects);
    kfree(snapshots);
}

error_t TagFS_CowInit(void)
{
    if (g_cow_state.initialized)
        return ERR_ALREADY_INITIALIZED;

    TagFSLockInit(&g_cow_state.lock, "cow", LOCK_RANK_COW);
    g_cow_state.snapshots      = NULL;
    g_cow_state.snapshot_count = 0;
    g_cow_state.initialized    = true;
    return OK;
}

void TagFS_CowShutdown(void)
{
    if (!g_cow_state.initialized)
        return;

    TagFSLockTake(&g_cow_state.lock);
    CowSnapshot *snapshots = g_cow_state.snapshots;
    uint32_t     count     = g_cow_state.snapshot_count;
    g_cow_state.snapshots      = NULL;
    g_cow_state.snapshot_count = 0;
    g_cow_state.initialized    = false;
    TagFSLockDrop(&g_cow_state.lock);

    CowSnapshotsFree(snapshots, count);
}

error_t TagFS_SnapshotRefused(const char *asked)
{
    if (!__atomic_exchange_n(&g_refusal_said, true, __ATOMIC_ACQ_REL))
        kprintf("[TagFS] %s is refused: snapshots arrive with format v2 — on format v1 a "
                "snapshot could be taken but never read, so every request for one is "
                "answered ERR_UNSUPPORTED\n", asked ? asked : "a snapshot request");
    return ERR_UNSUPPORTED;
}

// ----------------------------------------------------------------------------
// The snapshots an earlier kernel left, read at mount
// ----------------------------------------------------------------------------

/* The list being read, and whether its reading has failed: the replay hands
 * each redirect here, to the snapshot it names. */
typedef struct {
    CowSnapshot *snapshots;
    uint32_t     count;
    uint32_t     stray;          /* redirects naming no snapshot of the manifest */
    bool         short_of_memory;
} CowReading;

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

    for (uint32_t r = 0; r < snap->redirect_count; r++)
        if (snap->redirects[r].old_block == old_block &&
            snap->redirects[r].new_block == new_block)
            return;

    if (snap->redirect_count == snap->redirect_cap) {
        uint32_t     cap   = snap->redirect_cap ? snap->redirect_cap * 2 : 16;
        CowRedirect *grown = kmalloc(sizeof(CowRedirect) * cap);
        if (!grown) {
            reading->short_of_memory = true;
            return;
        }
        if (snap->redirect_count)
            memcpy(grown, snap->redirects, sizeof(CowRedirect) * snap->redirect_count);
        kfree(snap->redirects);
        snap->redirects    = grown;
        snap->redirect_cap = cap;
    }
    snap->redirects[snap->redirect_count++] = (CowRedirect){ old_block, new_block };
}

/* The manifest at `block`, read into a list of its own: NULL with *count 0
 * for a manifest that holds none, or one this mount could not read. */
static CowSnapshot *CowManifestRead(uint32_t block, uint32_t *count)
{
    *count = 0;
    CowManifest *manifest = kmalloc(sizeof(CowManifest));
    if (!manifest) {
        kprintf("[CoW] no memory to read the snapshot manifest at data block %u — the "
                "snapshots an earlier kernel left are not named, and the blocks they keep "
                "stay taken\n", block);
        return NULL;
    }
    if (tagfs_read_block(block, manifest) != OK ||
        manifest->magic != COW_MANIFEST_MAGIC || manifest->count > COW_MANIFEST_MAX) {
        kprintf("[CoW] the snapshot manifest the Ledger names at data block %u does not "
                "read as one — its snapshots are not named, and the blocks they keep stay "
                "taken\n", block);
        kfree(manifest);
        return NULL;
    }

    uint32_t     n         = manifest->count;
    CowSnapshot *snapshots = n ? kmalloc(sizeof(CowSnapshot) * n) : NULL;
    if (n && !snapshots) {
        kprintf("[CoW] no memory to hold the %u snapshot(s) the manifest at data block %u "
                "names — they are not named, and the blocks they keep stay taken\n",
                n, block);
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

/* One line for the lot and one for each, so a volume that carries them says
 * which, and that they are held and not lost. */
static void CowSayStanding(const CowReading *reading)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < reading->count; i++)
        kept += reading->snapshots[i].redirect_count;

    kprintf("[CoW] this volume carries %u snapshot(s) an earlier kernel took — held "
            "read-only, since snapshots arrive with format v2, and the %u block(s) they "
            "keep stay taken until the volume's census returns them\n",
            reading->count, kept);
    for (uint32_t i = 0; i < reading->count; i++) {
        const CowSnapshot *snap = &reading->snapshots[i];
        if (snap->parent_file_id)
            kprintf("[CoW]   '%s' (id %u) of file %u keeps %u block(s)\n", snap->name,
                    snap->snapshot_id, snap->parent_file_id, snap->redirect_count);
        else
            kprintf("[CoW]   '%s' (id %u) of the whole volume keeps %u block(s)\n",
                    snap->name, snap->snapshot_id, snap->redirect_count);
    }
    if (reading->stray)
        kprintf("[CoW] %u redirect(s) in the DiskBook name no snapshot of the manifest, "
                "and are passed over\n", reading->stray);
    if (reading->short_of_memory)
        kprintf("[CoW] memory ran out while the DiskBook was replayed — some blocks the "
                "snapshots keep are not named here, and stay taken only by the block "
                "bitmap\n");
}

void CowNameStanding(uint32_t manifest_block)
{
    if (!g_cow_state.initialized || manifest_block == 0)
        return;

    CowReading reading;
    memset(&reading, 0, sizeof(reading));
    reading.snapshots = CowManifestRead(manifest_block, &reading.count);
    if (reading.count == 0)
        return;

    if (DiskBookIsInitialized())
        DiskBookValidateAndReplay(CowRedirectSeen, &reading);

    TagFSLockTake(&g_cow_state.lock);
    g_cow_state.snapshots      = reading.snapshots;
    g_cow_state.snapshot_count = reading.count;
    TagFSLockDrop(&g_cow_state.lock);

    CowSayStanding(&reading);
}

/*
 * ‼ WHAT COPY-ON-WRITE OWNS, ANSWERED BY COPY-ON-WRITE ITSELF.
 *
 * The mount's roll call walks every file's extents and every metadata chain,
 * builds a map of what it found, and then keeps any block EITHER map calls
 * used. That union is what makes the repair additive and safe — nothing is
 * ever released over live metadata. But it has one hole, and the hole is a
 * block that NEITHER map names: one a kernel handed out and named in the
 * Ledger, whose bit had not yet reached the medium when the machine stopped.
 *
 * The manifest block is exactly that block. The kernel that took the first
 * snapshot allocated it and wrote the Ledger naming it at once, and the bit
 * saying it is taken lived in RAM until that kernel's next sync. Cut the power
 * in between and a mount reads a bitmap that calls the block FREE, a roll call
 * that does not reach it (no file's extent, no chain's magic), and a union
 * that therefore leaves it free — for the next writer to take, while the
 * Ledger still calls it the manifest.
 *
 * So CoW is asked, at the moment the roll call is taken, which blocks of the
 * data run are its own: the two manifest blocks the Ledger names, and both
 * blocks of every redirect the DiskBook replayed into a snapshot named at this
 * mount — the kept block no file names any more, and the one its file moved
 * onto. They stay taken until the volume's census, which knows every owner,
 * returns what nothing owns.
 *
 * Marks only. It never clears a bit, so it can only keep a block, never
 * release one.
 */
void CowRollCall(uint8_t *computed_bm, uint32_t total_blocks)
{
    if (!computed_bm)
        return;

    TagFSState *fs = tagfs_get_state();
    if (!fs)
        return;

#define COW_ROLL_CALL_MARK(blk) do {                                          \
        uint32_t _b = (blk);                                                  \
        if (_b != 0 && _b < (total_blocks))                                   \
            computed_bm[_b / 8] |= (uint8_t)(1u << (_b % 8));                 \
    } while (0)

    COW_ROLL_CALL_MARK(fs->ledger.cow_manifest_block);
    COW_ROLL_CALL_MARK(fs->ledger.cow_manifest_backup_block);

    /* Taken WITHOUT g_state.lock: the copy-on-write lock ranks above the
     * volume's, so it is never taken under it. */
    if (g_cow_state.initialized) {
        TagFSLockTake(&g_cow_state.lock);
        for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
            const CowSnapshot *snap = &g_cow_state.snapshots[i];
            for (uint32_t r = 0; r < snap->redirect_count; r++) {
                COW_ROLL_CALL_MARK(snap->redirects[r].old_block);
                COW_ROLL_CALL_MARK(snap->redirects[r].new_block);
            }
        }
        TagFSLockDrop(&g_cow_state.lock);
    }

#undef COW_ROLL_CALL_MARK
}
