#include "cow.h"
#include "../disk_book/disk_book.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_table/file_table.h"
#include "../tag_bitmap/tag_bitmap.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../tagfs.h"

// Global state
static CowState g_cow_state = { .lock = TAGFS_LOCK_INIT("cow", LOCK_RANK_COW) };

// Monotonic snapshot-ID source. File-scope (not a function-local static) so the
// on-disk restore path can seed it past every reloaded id — see below.
static volatile uint32_t g_next_snapshot_id = 1;

// Allocate new snapshot ID atomically
static uint32_t CowAllocSnapshotId(void) {
    return __atomic_fetch_add(&g_next_snapshot_id, 1, __ATOMIC_SEQ_CST);
}

// Seed the allocator past a restored snapshot id. Snapshots persist across
// reboots (CowWriteManifest) and are reloaded with their original ids, but the
// allocator restarts at 1 each boot — without this, a fresh CowAllocSnapshotId
// would hand out an id that collides with a reloaded snapshot, and snap_delete
// (which keys on id, first match) would then delete the WRONG snapshot. Bump
// the counter to max(current, id+1) so ids stay globally unique.
static void CowSeedSnapshotId(uint32_t id) {
    uint32_t cur = __atomic_load_n(&g_next_snapshot_id, __ATOMIC_SEQ_CST);
    while (id >= cur &&
           !__atomic_compare_exchange_n(&g_next_snapshot_id, &cur, id + 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        /* cur was reloaded with the live value on failure — retry. */
    }
}

// Write the in-memory snapshot list to the on-disk manifest block(s).
// Caller must hold g_cow_state.lock OR be in a context where the snapshot
// list is stable (e.g. after the lock is released but before returning).
// This function acquires no locks itself to avoid re-entrancy.
static void CowWriteManifest(void) {
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized)
        return;

    uint32_t primary_block = fs->ledger.cow_manifest_block;

    // Lazy allocation: on first snapshot, allocate a dedicated block for the manifest.
    if (primary_block == 0) {
        if (tagfs_alloc_blocks(1, &primary_block) != 0) {
            kprintf("[CoW] no free block for the snapshot manifest — the "
                    "snapshots taken since this volume was mounted exist in "
                    "memory only and will not survive a reboot\n");
            return;
        }
        fs->ledger.cow_manifest_block = primary_block;
        /* ‼ The Ledger is the only thing that names this block. If it refuses
         * the write, a block is taken in RAM that nothing on the medium will
         * ever claim — so it goes straight back, and the caller is told the
         * manifest is not on this volume rather than being left to assume it
         * is. */
        if (tagfs_write_ledger() != OK) {
            fs->ledger.cow_manifest_block = 0;
            tagfs_free_blocks(primary_block, 1);
            kprintf("[CoW] the ledger would not take the manifest block, so "
                    "the block is given back — the snapshots taken since this "
                    "volume was mounted exist in memory only\n");
            return;
        }
    }

    CowManifest *manifest = kmalloc(sizeof(CowManifest));
    if (!manifest)
        return;

    memset(manifest, 0, sizeof(CowManifest));
    manifest->magic = COW_MANIFEST_MAGIC;

    uint32_t count = g_cow_state.snapshot_count;
    if (count > COW_MANIFEST_MAX)
        count = COW_MANIFEST_MAX;
    manifest->count = count;

    for (uint32_t i = 0; i < count; i++) {
        CowSnapshot    *snap = &g_cow_state.snapshots[i];
        CowSnapshotDisk *dst = &manifest->entries[i];

        dst->snapshot_id         = snap->snapshot_id;
        dst->parent_file_id      = snap->parent_file_id;
        dst->created_time        = snap->created_time;
        dst->disk_book_checkpoint = (uint32_t)snap->disk_book_checkpoint;
        dst->file_count          = snap->file_count;
        dst->total_size          = snap->total_size;
        dst->flags               = snap->flags;
        memcpy(dst->name, snap->name, TAGFS_SNAPSHOT_NAME_LEN);
    }

    tagfs_write_block(primary_block, manifest);

    uint32_t backup_block = fs->ledger.cow_manifest_backup_block;
    if (backup_block != 0)
        tagfs_write_block(backup_block, manifest);

    kfree(manifest);
}

// Find snapshot by ID
static int CowFindSnapshot(uint32_t id) {
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (g_cow_state.snapshots[i].snapshot_id == id)
            return (int)i;
    }
    return -1;
}

// Append (old_block→new_block) to a snapshot's in-memory redirect list.
// De-duped (idempotent for replay), grows the array on demand. Caller holds
// g_cow_state.lock. Returns false only on allocation failure.
static bool cow_append_redirect_locked(CowSnapshot *snap,
                                       uint32_t old_block, uint32_t new_block) {
    for (uint32_t i = 0; i < snap->redirect_count; i++) {
        if (snap->redirects[i].old_block == old_block &&
            snap->redirects[i].new_block == new_block)
            return true;   /* already recorded */
    }
    if (snap->redirect_count >= snap->redirect_cap) {
        uint32_t new_cap = snap->redirect_cap ? snap->redirect_cap * 2 : 16;
        CowRedirect *grown = kmalloc(sizeof(CowRedirect) * new_cap);
        if (!grown)
            return false;
        if (snap->redirects && snap->redirect_count > 0)
            memcpy(grown, snap->redirects, sizeof(CowRedirect) * snap->redirect_count);
        if (snap->redirects)
            kfree(snap->redirects);
        snap->redirects    = grown;
        snap->redirect_cap = new_cap;
    }
    snap->redirects[snap->redirect_count].old_block = old_block;
    snap->redirects[snap->redirect_count].new_block = new_block;
    snap->redirect_count++;
    return true;
}

// Compact the durable redirect log after a snapshot is deleted: drop all
// on-disk redirects and re-log the survivors. Deleted snapshots' redirects are
// harmless on replay (no matching snapshot) but waste journal slots — this
// reclaims them so the bounded log doesn't fill prematurely.
static void cow_resync_redirect_journal(void) {
    TagFSLockTake(&g_cow_state.lock);
    DiskBookResetRedirects();
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        CowSnapshot *s = &g_cow_state.snapshots[i];
        for (uint32_t r = 0; r < s->redirect_count; r++) {
            DiskBookLogRedirect(s->snapshot_id,
                                s->redirects[r].old_block,
                                s->redirects[r].new_block, 0);
        }
    }
    TagFSLockDrop(&g_cow_state.lock);
}

error_t TagFS_CowInit(void) {
    if (g_cow_state.initialized)
        return ERR_ALREADY_INITIALIZED;
    
    memset(&g_cow_state, 0, sizeof(CowState));
    TagFSLockInit(&g_cow_state.lock, "cow", LOCK_RANK_COW);
    
    g_cow_state.snapshot_capacity = TAGFS_MAX_SNAPSHOTS;
    g_cow_state.snapshots = kmalloc(sizeof(CowSnapshot) * g_cow_state.snapshot_capacity);
    if (!g_cow_state.snapshots)
        return ERR_NO_MEMORY;
    
    memset(g_cow_state.snapshots, 0, sizeof(CowSnapshot) * g_cow_state.snapshot_capacity);
    g_cow_state.magic = COW_MAGIC;
    g_cow_state.version = COW_VERSION;
    g_cow_state.initialized = true;
    
    debug_printf("[CoW] Initialized: %u snapshots\n", g_cow_state.snapshot_capacity);
    return OK;
}

void TagFS_CowShutdown(void) {
    if (!g_cow_state.initialized)
        return;
    
    TagFSLockTake(&g_cow_state.lock);
    if (g_cow_state.snapshots) {
        kfree(g_cow_state.snapshots);
        g_cow_state.snapshots = NULL;
    }
    g_cow_state.initialized = false;
    TagFSLockDrop(&g_cow_state.lock);
    
    debug_printf("[CoW] Shutdown complete\n");
}

error_t TagFS_SnapshotCreate(const char *name, uint32_t file_id, uint32_t *snapshot_id) {
    if (!g_cow_state.initialized || !name || !snapshot_id)
        return ERR_INVALID_ARGUMENT;

    TagFSLockTake(&g_cow_state.lock);

    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }

    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (strcmp(g_cow_state.snapshots[i].name, name) == 0) {
            *snapshot_id = g_cow_state.snapshots[i].snapshot_id;
            TagFSLockDrop(&g_cow_state.lock);
            return ERR_ALREADY_EXISTS;
        }
    }

    uint32_t slot_index = g_cow_state.snapshot_count;
    CowSnapshot *snap = &g_cow_state.snapshots[slot_index];
    memset(snap, 0, sizeof(CowSnapshot));

    snap->snapshot_id = CowAllocSnapshotId();
    snap->parent_file_id = file_id;
    snap->created_time = rtc_get_unix64();
    snap->disk_book_checkpoint = DiskBookGetCheckpointSeq();
    strncpy(snap->name, name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = COW_SNAP_READONLY;

    TagFSMetadata file_meta;
    memset(&file_meta, 0, sizeof(file_meta));
    bool has_file_meta = false;

    if (file_id == 0) {
        TagFSState *fs = tagfs_get_state();
        if (fs)
            snap->file_count = fs->ledger.total_files;
    } else {
        if (tagfs_get_metadata(file_id, &file_meta) == OK) {
            snap->file_count = 1;
            snap->total_size = file_meta.size;
            snap->block_count = 0;
            for (uint16_t j = 0; j < file_meta.extent_count; j++)
                snap->block_count += file_meta.extents[j].block_count;
            has_file_meta = true;
        } else {
            TagFSLockDrop(&g_cow_state.lock);
            return ERR_FILE_NOT_FOUND;
        }
    }

    // Commit snapshot to journal before making it visible.
    // Use the file metadata for the journal record so the log is real.
    // For whole-fs snapshots, build a dummy metadata record with snapshot_id as file_id.
    g_cow_state.snapshot_count++;
    g_cow_state.snapshots_created++;
    *snapshot_id = snap->snapshot_id;

    TagFSLockDrop(&g_cow_state.lock);

    /* Snapshot descriptors persist via the CoW manifest (CowWriteManifest).
     * The old per-snapshot DiskBook log serialized an in-memory TagFSMetadata
     * (heap pointers) by memcpy — meaningless after reboot — so it is gone.
     * DiskBook now carries only what the manifest cannot: the per-block CoW
     * redirect map, logged from TagFS_CowAfterWrite. */
    if (has_file_meta)
        tagfs_metadata_free(&file_meta);

    CowWriteManifest();

    debug_printf("[CoW] Created '%s' (id=%u, files=%u)\n", name, snap->snapshot_id, snap->file_count);
    return OK;
}

error_t TagFS_SnapshotCreateByTag(const char *tag_pattern, uint32_t *snapshot_id) {
    if (!g_cow_state.initialized || !tag_pattern || !snapshot_id)
        return ERR_INVALID_ARGUMENT;
    
    TagFSLockTake(&g_cow_state.lock);
    
    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }
    
    char snap_name[TAGFS_SNAPSHOT_NAME_LEN];
    ksnprintf(snap_name, sizeof(snap_name), "tag-%s", tag_pattern);
    
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (strcmp(g_cow_state.snapshots[i].name, snap_name) == 0) {
            *snapshot_id = g_cow_state.snapshots[i].snapshot_id;
            TagFSLockDrop(&g_cow_state.lock);
            return ERR_ALREADY_EXISTS;
        }
    }

    CowSnapshot *snap = &g_cow_state.snapshots[g_cow_state.snapshot_count];
    memset(snap, 0, sizeof(CowSnapshot));
    
    snap->snapshot_id = CowAllocSnapshotId();
    snap->parent_file_id = 0;
    snap->created_time = rtc_get_unix64();
    snap->disk_book_checkpoint = DiskBookGetCheckpointSeq();
    strncpy(snap->name, snap_name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = COW_SNAP_READONLY | COW_SNAP_TAG_QUERY;
    
    uint32_t file_count = 0;
    uint64_t total_size = 0;
    uint32_t block_count = 0;
    
    uint32_t *file_ids = kmalloc(sizeof(uint32_t) * TAGFS_MAX_FILES);
    if (file_ids) {
        int count = tagfs_list_all_files(file_ids, TAGFS_MAX_FILES);
        for (int i = 0; i < count; i++) {
            file_count++;
            TagFSMetadata meta;
            if (tagfs_get_metadata(file_ids[i], &meta) == OK) {
                total_size += meta.size;
                for (uint16_t j = 0; j < meta.extent_count; j++)
                    block_count += meta.extents[j].block_count;
                tagfs_metadata_free(&meta);
            }
        }
        kfree(file_ids);
    }
    
    snap->file_count = file_count;
    snap->total_size = total_size;
    snap->block_count = block_count;
    
    g_cow_state.snapshot_count++;
    g_cow_state.snapshots_created++;
    *snapshot_id = snap->snapshot_id;

    TagFSLockDrop(&g_cow_state.lock);

    CowWriteManifest();

    debug_printf("[CoW] Created tag snapshot '%s' (id=%u, files=%u)\n", snap_name, snap->snapshot_id, file_count);
    return OK;
}

error_t TagFS_SnapshotDelete(uint32_t snapshot_id) {
    if (!g_cow_state.initialized)
        return ERR_NOT_INITIALIZED;

    TagFSLockTake(&g_cow_state.lock);

    int slot = CowFindSnapshot(snapshot_id);
    if (slot < 0) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }

    /* Snapshot is going away — its frozen view no longer needs the
     * preserved OLD block contents. Release every redirected OLD
     * block back to the allocator. NEW blocks remain (the live file
     * uses them). */
    CowSnapshot *snap = &g_cow_state.snapshots[slot];
    uint32_t freed = 0;
    if (snap->redirects && snap->redirect_count > 0) {
        for (uint32_t i = 0; i < snap->redirect_count; i++) {
            uint32_t ob = snap->redirects[i].old_block;
            if (ob != 0) {
                tagfs_free_blocks(ob, 1);
                freed++;
            }
        }
    }
    if (snap->redirects) {
        kfree(snap->redirects);
        snap->redirects     = NULL;
        snap->redirect_count = 0;
        snap->redirect_cap  = 0;
    }

    for (uint32_t i = slot; i < g_cow_state.snapshot_count - 1; i++)
        g_cow_state.snapshots[i] = g_cow_state.snapshots[i + 1];

    g_cow_state.snapshot_count--;
    TagFSLockDrop(&g_cow_state.lock);

    CowWriteManifest();
    cow_resync_redirect_journal();   /* reclaim the deleted snapshot's redirect slots */

    debug_printf("[CoW] Deleted snapshot %u (freed %u redirected blocks)\n",
                 snapshot_id, freed);
    return OK;
}

error_t TagFS_SnapshotList(uint32_t *ids, uint32_t max_count, uint32_t *actual_count) {
    if (!g_cow_state.initialized || !ids || !actual_count)
        return ERR_INVALID_ARGUMENT;
    
    TagFSLockTake(&g_cow_state.lock);
    
    uint32_t count = max_count < g_cow_state.snapshot_count ? max_count : g_cow_state.snapshot_count;
    for (uint32_t i = 0; i < count; i++)
        ids[i] = g_cow_state.snapshots[i].snapshot_id;
    
    *actual_count = count;
    TagFSLockDrop(&g_cow_state.lock);
    return OK;
}

error_t TagFS_SnapshotInfo(uint32_t snapshot_id, CowSnapshot *info) {
    if (!g_cow_state.initialized || !info)
        return ERR_INVALID_ARGUMENT;
    
    TagFSLockTake(&g_cow_state.lock);
    
    int slot = CowFindSnapshot(snapshot_id);
    if (slot < 0) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }
    
    memcpy(info, &g_cow_state.snapshots[slot], sizeof(CowSnapshot));
    TagFSLockDrop(&g_cow_state.lock);
    return OK;
}

error_t TagFS_CowBeforeWrite(uint32_t file_id, uint32_t block_addr, uint32_t *new_block) {
    (void)file_id;  // Reserved for future per-file COW policies
    if (!g_cow_state.initialized || !new_block || block_addr == 0)
        return ERR_INVALID_ARGUMENT;

    TagFSLockTake(&g_cow_state.lock);

    // Allocate new block for COW
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_NOT_INITIALIZED;
    }

    // Read original block data
    uint8_t original_data[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block_addr, original_data) != OK) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_READ_FAILED;
    }

    // Allocate new block
    uint32_t free_block = 0;
    int alloc_result = tagfs_alloc_blocks(1, &free_block);
    if (alloc_result != OK || free_block == 0) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }

    // Copy original data to new block
    if (tagfs_write_block(free_block, original_data) != OK) {
        tagfs_free_blocks(free_block, 1);
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_WRITE_FAILED;
    }

    // Return new block address
    *new_block = free_block;
    g_cow_state.cow_copies++;

    TagFSLockDrop(&g_cow_state.lock);
    return OK;
}

error_t TagFS_CowAfterWrite(uint32_t file_id, uint32_t old_block, uint32_t new_block) {
    if (!g_cow_state.initialized)
        return ERR_NOT_INITIALIZED;
    if (old_block == 0 || new_block == 0)
        return ERR_INVALID_ARGUMENT;

    TagFSLockTake(&g_cow_state.lock);

    /* Find the snapshot whose frozen view should keep this OLD block. */
    CowSnapshot *snap = NULL;
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        CowSnapshot *s = &g_cow_state.snapshots[i];
        if (s->parent_file_id == file_id) { snap = s; break; }
        if (s->parent_file_id == 0 && !snap) snap = s;
    }

    if (!snap) {
        /* No snapshot found at AfterWrite time. Two contexts produce
         * this: (1) a unit-test calling the API directly with arbitrary
         * blocks (we MUST NOT touch the bitmap — those blocks may be
         * reserved/chain blocks), (2) a snapshot disappeared between
         * Before and After (rare). Either way, leave old_block alone;
         * the block accounting is the caller's responsibility. */
        g_cow_state.cow_writes++;
        TagFSLockDrop(&g_cow_state.lock);
        return OK;
    }

    if (!cow_append_redirect_locked(snap, old_block, new_block)) {
        TagFSLockDrop(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }

    g_cow_state.cow_writes++;
    g_cow_state.blocks_shared++;

    /* Mirror the redirect into the durable DiskBook log so this snapshot's
     * frozen view of old_block survives a power cut — the in-memory list above
     * is RAM-only and lost on reboot. Best-effort: on a full log or I/O error
     * the block gracefully falls back to live data after crash recovery, never
     * corruption. */
    DiskBookLogRedirect(snap->snapshot_id, old_block, new_block, 0);

    TagFSLockDrop(&g_cow_state.lock);
    return OK;
}

void TagFS_CowGetStats(uint64_t *cow_writes, uint64_t *cow_copies,
                       uint64_t *blocks_shared, uint32_t *snapshots) {
    if (!g_cow_state.initialized)
        return;
    
    TagFSLockTake(&g_cow_state.lock);
    if (cow_writes) *cow_writes = g_cow_state.cow_writes;
    if (cow_copies) *cow_copies = g_cow_state.cow_copies;
    if (blocks_shared) *blocks_shared = g_cow_state.blocks_shared;
    if (snapshots) *snapshots = g_cow_state.snapshots_created;
    TagFSLockDrop(&g_cow_state.lock);
}

bool TagFS_CowIsActive(uint32_t file_id) {
    if (!g_cow_state.initialized)
        return false;

    TagFSLockTake(&g_cow_state.lock);

    // CoW is active if there are any snapshots for this file
    // or if file has the COW flag set
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        CowSnapshot *snap = &g_cow_state.snapshots[i];
        if (snap->parent_file_id == file_id || snap->parent_file_id == 0) {
            TagFSLockDrop(&g_cow_state.lock);
            return true;
        }
    }

    // Also check if file has COW flag in metadata
    TagFSMetadata meta;
    if (tagfs_get_metadata(file_id, &meta) == OK) {
        bool is_cow = (meta.flags & TAGFS_FILE_COW) != 0;
        tagfs_metadata_free(&meta);
        TagFSLockDrop(&g_cow_state.lock);
        return is_cow;
    }

    TagFSLockDrop(&g_cow_state.lock);
    return false;
}

uint64_t TagFS_CowGetCheckpoint(void) {
    return g_cow_state.initialized ? DiskBookGetCheckpointSeq() : 0;
}

void TagFS_CowRestoreSnapshot(const CowSnapshot *snap) {
    if (!g_cow_state.initialized || !snap)
        return;

    /* Keep the allocator ahead of every reloaded id (unique-id invariant). */
    CowSeedSnapshotId(snap->snapshot_id);

    TagFSLockTake(&g_cow_state.lock);

    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        TagFSLockDrop(&g_cow_state.lock);
        debug_printf("[CoW] RestoreSnapshot: no capacity for snapshot_id=%u\n", snap->snapshot_id);
        return;
    }

    // Skip if already present (idempotent restore)
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (g_cow_state.snapshots[i].snapshot_id == snap->snapshot_id) {
            TagFSLockDrop(&g_cow_state.lock);
            return;
        }
    }

    CowSnapshot *dst = &g_cow_state.snapshots[g_cow_state.snapshot_count];
    memcpy(dst, snap, sizeof(CowSnapshot));
    /* Reset runtime redirect fields — they are restored separately, after this,
     * by DiskBookValidateAndReplay() → TagFS_CowRestoreRedirect(). */
    dst->redirects      = NULL;
    dst->redirect_count = 0;
    dst->redirect_cap   = 0;
    g_cow_state.snapshot_count++;

    TagFSLockDrop(&g_cow_state.lock);
}

void TagFS_CowRestoreRedirect(uint32_t snapshot_id, uint32_t old_block, uint32_t new_block) {
    if (!g_cow_state.initialized || old_block == 0 || new_block == 0)
        return;
    TagFSLockTake(&g_cow_state.lock);
    int slot = CowFindSnapshot(snapshot_id);
    if (slot >= 0)
        cow_append_redirect_locked(&g_cow_state.snapshots[slot], old_block, new_block);
    /* snapshot not found (deleted before this redirect was replayed) → ignore */
    TagFSLockDrop(&g_cow_state.lock);
}

/*
 * ‼ WHAT COPY-ON-WRITE OWNS, ANSWERED BY COPY-ON-WRITE ITSELF.
 *
 * The mount's roll call walks every file's extents and every metadata chain,
 * builds a map of what it found, and then keeps any block EITHER map calls
 * used. That union is what makes the repair additive and safe — nothing is
 * ever released over live metadata. But it has one hole, and the hole is a
 * block that NEITHER map names: one the kernel handed out and named in the
 * Ledger, whose bit had not yet reached the medium when the machine stopped.
 *
 * The manifest block is exactly that block. CowWriteManifest allocates it on
 * the first snapshot, writes the Ledger naming it immediately, and the bit
 * saying it is taken lives in RAM until the next tagfs_sync. Cut the power in
 * between and the next mount reads a bitmap that calls the block FREE, a roll
 * call that does not reach it (no file's extent, no chain's magic), and a
 * union that therefore leaves it free. The free list hands it to the next
 * writer, and one block now has two owners: whichever of them writes second
 * destroys what the other put there.
 *
 * So CoW is asked, at the moment the roll call is taken, which blocks of the
 * data run are its own. Said plainly, because it bounds what this answers:
 * the two manifest blocks are named by the Ledger and are therefore known on
 * every mount; the per-snapshot redirects are a RUNTIME list that comes back
 * empty after a reboot (see the note over CowSnapshot), so on a cold mount
 * this answers with the manifest and nothing more. That is not a shortfall
 * here — a redirect's OLD block was already marked used on the medium by the
 * write that preserved it, and its NEW block is reachable through the file
 * that owns it. The manifest is the one block in this subsystem with no other
 * way to be found.
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

    /* The redirects are held under the CoW lock, and this is taken WITHOUT
     * g_state.lock — the same order CowWriteManifest already establishes
     * (CoW lock first, then the volume's), so a roll call cannot invert it. */
    if (g_cow_state.initialized) {
        TagFSLockTake(&g_cow_state.lock);
        for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
            CowSnapshot *snap = &g_cow_state.snapshots[i];
            if (!snap->redirects)
                continue;
            for (uint32_t r = 0; r < snap->redirect_count; r++) {
                COW_ROLL_CALL_MARK(snap->redirects[r].old_block);
                COW_ROLL_CALL_MARK(snap->redirects[r].new_block);
            }
        }
        TagFSLockDrop(&g_cow_state.lock);
    }

#undef COW_ROLL_CALL_MARK
}
