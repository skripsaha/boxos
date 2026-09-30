#include "mount.h"
#include "ground_census.h"
#include "../tag_registry/tag_registry.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../metadata_pool/meta_pool.h"
#include "../block_map/block_map.h"
#include "../ledger/ledger.h"
#include "../settle/settle.h"
#include "../integrity/integrity.h"
#include "../cow/cow.h"
#include "../use_memory/use_memory.h"

/*
 * The mount's read of the volume (mount.h). Everything here runs with the
 * door shut, so the volume's state is nobody else's yet and is written with no
 * lock; the maps it fills take their own leaf locks as they always do.
 */

/* What MountAfterDoor owes the integrity map: the files the stood seal's own
 * cycle wrote records of, when that cycle was not a machine stopping. */
static uint32_t* s_last_span;
static uint32_t  s_last_span_count;
static uint64_t  s_unclean_cycle;

/* A block the Deed names → the block TagFS allocates in. The Deed states where
 * the registry, the file table, the pool and the BOOT run are in blocks of the
 * whole volume — what makes a volume readable by something that has not
 * mounted it; TagFS counts from the start of the data run, as every extent on
 * the medium is counted. This is the seam. */
static uint32_t MountDataBlock(uint32_t volume_block)
{
    return volume_block - tagfs_get_state()->layout.data_block;
}

// ----------------------------------------------------------------------------
// The seals
// ----------------------------------------------------------------------------

typedef struct {
    SettleMark mark[VOLUME_LEDGER_COPIES];
    bool       has[VOLUME_LEDGER_COPIES];
    uint32_t   copy[VOLUME_LEDGER_COPIES];     /* [0] the newest intact copy, [1] the other */
} MountSeals;

/*
 * ‼ THE SEAL IS TAKEN FROM THE NEWEST COPY THAT READS, OR NOT AT ALL.
 *
 * A newest copy with no seal of its own was written last by a kernel before
 * the seal — its record carries a Use Context where the seal would be, or ends
 * before it (volume_ledger.h, LedgerHas). What the other copy's seal says was
 * true before that kernel wrote, and that kernel may have rewritten closed
 * blocks since; so the volume is read the way that kernel reads it, and the
 * rules of the closed block and of Fallow begin again with the first cycle.
 */
static void MountSealsRead(MountSeals* seals)
{
    memset(seals, 0, sizeof(*seals));
    seals->copy[0] = LedgerNewestCopy();
    seals->copy[1] = (seals->copy[0] + 1u) % VOLUME_LEDGER_COPIES;
    seals->has[0]  = LedgerCopySeal(seals->copy[0], &seals->mark[0]);
    seals->has[1]  = seals->has[0] && LedgerCopySeal(seals->copy[1], &seals->mark[1]);

    if (!seals->has[0])
        kprintf("[TagFS] this volume's newest Ledger carries no seal: a kernel before the "
                "seal wrote it last, so it is read as that kernel left it — its file table "
                "says which files there are, its pool is read to the end — and the first "
                "Settle cycle of this kernel seals it\n");
}

// ----------------------------------------------------------------------------
// The files
// ----------------------------------------------------------------------------

typedef struct {
    bool     sealed;
    uint32_t checked;
    uint32_t corrected;      /* a row naming another record than the newest */
    uint32_t derived;        /* sealed: a row the table lacked, set from the pool */
    uint32_t unsettled;      /* sealed: a row with no record in the span */
    uint32_t lost;           /* no seal: a row with no record that reads */
    uint32_t unnamed;        /* no seal: a record no row names */
    uint32_t top_fid;
} MountFileCount;

/*
 * One file the map holds — a row, a record, or both — judged by the rules the
 * volume stands on, and put into the tag index and the census.
 *
 * ‼ WHERE THE POOL SAYS THE NEWEST RECORD IS, WINS. The pool is only appended
 * to and the walk read it in order, so the record the map holds is the newest
 * one of the file the walk reached. A row naming somewhere else is corrected
 * to it, in memory now and on the medium by the next cycle.
 */
static void MountFile(uint32_t fid, MountFileCount* n)
{
    TagFSState* fs = tagfs_get_state();
    RecordWhere where;
    uint32_t    mb = 0, mo = 0;
    FileRecord* rec     = FileMapRecordHold(fs->files, fid, &where);
    bool        has_row = file_table_lookup(fid, &mb, &mo) == 0;

    if (fid > n->top_fid)
        n->top_fid = fid;

    if (!rec) {
        if (n->sealed) {
            n->unsettled++;
        } else {
            /* Said with kprintf, so it survives into a shipped build: this
             * line was the silence that turned a lost file into a mystery. */
            kprintf("[TagFS FSCK] file %u is named at block %u+%u and this volume's pool "
                    "holds no record of it that reads — the file is gone and its name "
                    "goes with it\n", fid, mb, mo);
            n->lost++;
        }
        file_table_delete(fid);
        FileMapDrop(fs->files, fid);
        return;
    }
    if (!has_row && !n->sealed) {
        n->unnamed++;
        FileRecordRelease(rec);
        FileMapDrop(fs->files, fid);
        return;
    }

    uint32_t pb = 0;
    if (PoolLinkBlock(where.link, &pb) && (!has_row || pb != mb || where.offset != mo)) {
        file_table_update(fid, pb, where.offset);
        if (has_row)
            n->corrected++;
        else
            n->derived++;
    }
    for (uint32_t e = 0; e < rec->extent_count; e++)
        GroundClaim(GROUND_FILE, rec->extents[e].start_block, rec->extents[e].block_count, fid);
    for (uint32_t t = 0; t < rec->tag_count; t++)
        tag_bitmap_set(fs->bitmap_index, rec->tags[t], fid);
    n->checked++;
    FileRecordRelease(rec);
}

/* Every file the map holds, a batch of ids at a time: no lock is held while
 * one is judged, which may take it out of the map. */
static void MountWalkFiles(MountFileCount* n)
{
    TagFSState* fs = tagfs_get_state();
    uint32_t    batch[64];
    uint32_t    from = 1;
    uint32_t    got;
    while ((got = FileMapFiles(fs->files, from, batch, ARRAY_SIZE(batch))) > 0) {
        for (uint32_t i = 0; i < got; i++)
            MountFile(batch[i], n);
        if (batch[got - 1] == 0xFFFFFFFFu)
            break;
        from = batch[got - 1] + 1;
    }

    /* Said aloud: a corrected row, a lost file and a let-go row are all
     * things that happened to the person's files. */
    if (n->corrected || n->lost)
        kprintf("[TagFS FSCK] %u file table entry(s) corrected from this volume's metadata "
                "pool, %u file(s) lost\n", n->corrected, n->lost);
    if (n->unsettled || n->derived)
        kprintf("[TagFS] the file table is derived from the sealed pool: %u row(s) named "
                "files of a cycle that did not settle and are let go, %u row(s) the table "
                "lacked are set from the pool\n", n->unsettled, n->derived);
    if (n->unnamed)
        kprintf("[TagFS FSCK] %u ACTIVE record(s) of the pool no table row names are not "
                "taken up — a create a cut stopped between the pool and the table; the "
                "census names their blocks\n", n->unnamed);
}

// ----------------------------------------------------------------------------
// The numbers the medium has seen
// ----------------------------------------------------------------------------

/*
 * ‼ THE NEXT FILE ID IS DERIVED, AND IT IS PAST EVERY ID THE MEDIUM SHOWED.
 *
 * The Ledger's next_file_id is written by the cycle's S4, after the table
 * (S1) and the pool (S3): a cut between them left a row, or a record, naming
 * an id the Ledger had not counted yet, and the next create was handed that id
 * again — its row written over the other's (nextfid). So the boundary is the
 * highest id of every row, of every record the walk met — past the seal too,
 * a cycle that did not settle — and the Ledger's own count, whichever is
 * furthest. An id past 0xFFFFFFFE is every id issued (LedgerIssueFileId).
 */
uint32_t MountNextFileId(uint32_t ledger_next, uint32_t top_row, uint32_t top_record)
{
    uint64_t top  = top_row > top_record ? top_row : top_record;
    uint64_t next = top + 1u;
    if (next > UINT32_MAX)
        next = UINT32_MAX;
    return ledger_next == 0 || next <= ledger_next ? ledger_next : (uint32_t)next;
}

static void MountFidBoundary(uint32_t top_row, uint32_t top_record)
{
    VolumeLedger* led  = &tagfs_get_state()->ledger;
    uint32_t      next = MountNextFileId(led->next_file_id, top_row, top_record);
    if (next != led->next_file_id) {
        kprintf("[TagFS] the Ledger's next file id is %u and the medium names file id %u — "
                "the ids from %u on were seen by a cycle the Ledger did not count, so the next "
                "file is given %u, and none of them again\n", led->next_file_id,
                top_row > top_record ? top_row : top_record, led->next_file_id, next);
        led->next_file_id = next;
    }
}

// ----------------------------------------------------------------------------
// The census
// ----------------------------------------------------------------------------

/* The BOOT run the Deed names for the loader is the kernel file's own blocks,
 * named twice; it is an owner in its own right, so a kernel file a cut took
 * from the pool still keeps the blocks the loader reads (RT-09). */
static void MountClaimBoot(void)
{
    TagFSState*       fs   = tagfs_get_state();
    const VolumeBoot* boot = &fs->boot;
    if (boot->kernel_blocks && boot->kernel_block >= fs->layout.data_block)
        GroundClaim(GROUND_BOOT, MountDataBlock(boot->kernel_block), boot->kernel_blocks, 0);
}

/*
 * ‼ THE FREE COUNT IS DERIVED, NOT REMEMBERED.
 *
 * The Ledger carries a count of free blocks, and it used to be believed. It is
 * the one number on a volume no single write makes true, so anything that
 * interrupts a sequence of writes leaves it saying something the map does
 * not. MEASURED 2026-09-10: a volume that grew, had its head deed rolled back
 * and grew again came up claiming 118764 free blocks in a data run of 73594.
 * The census has just counted the map; that is the number kept, and the next
 * Settle cycle writes it — a mount writes no Ledger.
 */
static error_t MountCensus(const char** what)
{
    TagFSState* fs      = tagfs_get_state();
    uint64_t    as_read = fs->ledger.free_blocks;

    TagRegistryClaimGround(fs->registry);
    FileTableClaimGround();
    PoolClaimGround(MountDataBlock(fs->layout.metadata_pool_block));
    IntegrityClaimGround();
    CowClaimGround(fs->ledger.cow_manifest_block);
    UseRollCall();
    MountClaimBoot();

    /* The orphans are asked for only when the newest copy of the Ledger was
     * written by a kernel that does not know the Use Context's chain: one it
     * stopped naming is among them (use_memory.h). */
    BlockMapCensusCount census;
    error_t rc = GroundCensusRun(UseMemoryWantsOrphans() ? UseMemoryOrphanRun : NULL, &census);
    GroundCensusEnd();
    if (rc == ERR_TAGFS_CORRUPTED) {
        /*
         * ‼ A bitmap that would not read is NOT an empty bitmap: it says every
         * block is free, and the next allocation hands out the registry, the
         * table and the pool. Measured on the board: a flash drive that dropped
         * a command mid-boot came up once working, and the boot after had no
         * files at all. Refusing costs the machine its filesystem for a boot;
         * carrying on costs it the filesystem.
         */
        *what = "its block bitmap would not read, and a missing bitmap reads as a volume "
                "with nothing on it — the next write would take everything";
        return rc;
    }
    if (rc != OK) {
        *what = "there is no memory to hold its block map's repair";
        return rc;
    }
    fs->ledger.free_blocks = census.free;
    if (census.free != as_read)
        kprintf("[TagFS] the ledger came off the medium saying %llu blocks were free and the "
                "bitmap says %llu — a block is taken in the bitmap, so that is the number "
                "kept, and the next Settle cycle writes it\n",
                (unsigned long long)as_read, (unsigned long long)census.free);
    UseMemoryAfterCensus();
    return OK;
}

// ----------------------------------------------------------------------------
// The whole read
// ----------------------------------------------------------------------------

static error_t MountRegistry(const MountSeals* seals, const char** what)
{
    TagFSState* fs = tagfs_get_state();
    fs->registry   = kmalloc(sizeof(TagRegistry));
    if (!fs->registry) {
        *what = "there is no memory for its tag registry";
        return ERR_NO_MEMORY;
    }
    if (tag_registry_init(fs->registry) != OK) {
        kfree(fs->registry);
        fs->registry = NULL;
        *what = "its tag registry would not be built";
        return ERR_TAGFS_REGISTRY_FULL;
    }
    /* A registry that would not read is not an empty registry: every tag id
     * on this volume is defined there, so a mount that continues without it
     * renames nothing and mislabels everything. */
    if (tag_registry_load(fs->registry, MountDataBlock(fs->layout.tag_registry_block),
                          seals->has[0] ? &seals->mark[0] : NULL,
                          seals->has[1] ? &seals->mark[1] : NULL) != OK) {
        *what = "its tag registry would not read";
        return ERR_TAGFS_CORRUPTED;
    }
    return OK;
}

/* The pool walked, and the copy of the Ledger it stood on taken up. */
static error_t MountPool(const MountSeals* seals, PoolMountOutcome* pool, const char** what)
{
    TagFSState*  fs  = tagfs_get_state();
    PoolMountAsk ask = {
        .seal = { seals->has[0] ? &seals->mark[0] : NULL, seals->has[1] ? &seals->mark[1] : NULL },
        .head = MountDataBlock(fs->layout.metadata_pool_block),
    };
    error_t rc = PoolMount(&ask, pool);
    if (rc != OK) {
        *what = rc == ERR_NO_MEMORY ? "its files' records could not all be held in memory"
                                    : "its metadata pool would not read";
        return rc;
    }

    /* The copy stood on: the seal's own, or the newest when none stands. */
    uint32_t stood = pool->seal >= 0 ? seals->copy[pool->seal] : seals->copy[0];
    LedgerStandOn(stood);

    bool lied = pool->fell_back || UseMemoryMediumLied() ||
                (pool->seal >= 0 && (seals->mark[pool->seal].flags & SETTLE_MEDIUM_LIED));
    SettleMountStand(pool->from_block, pool->from_bytes, lied);

    /* RT-22: what the stood seal's own cycle wrote, when that cycle was not
     * a machine stopping — kept for MountAfterDoor. A mount refused after
     * this point left its list behind; this one lets it go. */
    kfree(s_last_span);
    s_last_span       = NULL;
    s_last_span_count = 0;
    s_unclean_cycle   = 0;
    if (pool->seal >= 0 && !(seals->mark[pool->seal].flags & SETTLE_CLEAN)) {
        s_last_span       = pool->last_span;
        s_last_span_count = pool->last_span_count;
        s_unclean_cycle   = seals->mark[pool->seal].settle;
        pool->last_span   = NULL;
    }
    kfree(pool->last_span);
    pool->last_span = NULL;
    return OK;
}

/* The part of the bitmap that covers the data run is the part read: the
 * region is laid down for the ground the volume may one day take. A region too
 * small for a bit per data block was never made by mkfs — a damaged Deed. */
static error_t MountBlockMap(const char** what)
{
    TagFSState* fs      = tagfs_get_state();
    uint32_t    region  = fs->layout.block_bitmap_blocks * TAGFS_BLOCK_SECTORS;
    uint32_t    sectors = (fs->layout.data_blocks + TAGFS_SECTOR_SIZE * 8 - 1) / (TAGFS_SECTOR_SIZE * 8);
    if (sectors > region) {
        *what = "its block bitmap region is too small to hold a bit for every block of its "
                "data run";
        return ERR_TAGFS_CORRUPTED;
    }
    if (BlockMapMount(fs->layout.data_blocks,
                      (uint64_t)fs->layout.block_bitmap_block * TAGFS_BLOCK_SECTORS) != OK) {
        *what = "there is no memory for its block map";
        return ERR_NO_MEMORY;
    }
    return OK;
}

error_t MountReadVolume(const char** what)
{
    TagFSState* fs = tagfs_get_state();
    MountSeals  seals;
    MountSealsRead(&seals);

    error_t rc = MountRegistry(&seals, what);
    if (rc != OK)
        return rc;

    fs->files = FileMapCreate();
    if (!fs->files) {
        *what = "there is no memory for its map of live files";
        return ERR_NO_MEMORY;
    }

    PoolMountOutcome pool;
    rc = MountPool(&seals, &pool, what);
    if (rc != OK)
        return rc;

    MountFileCount files = { .sealed = pool.stood != POOL_STOOD_NO_SEAL };
    if (file_table_init(MountDataBlock(fs->layout.file_table_block), files.sealed) != OK) {
        *what = "its file table would not read";
        return ERR_FILE_TABLE_CORRUPT;
    }

    fs->bitmap_index = tag_bitmap_create(TAGFS_BITMAP_INITIAL_TAG_CAP);
    if (!fs->bitmap_index) {
        *what = "there is no memory for its tag index";
        return ERR_NO_MEMORY;
    }
    rc = MountBlockMap(what);
    if (rc != OK)
        return rc;

    GroundCensusBegin();
    MountWalkFiles(&files);
    fs->ledger.total_files = files.checked;
    MountFidBoundary(files.top_fid, pool.top_fid);
    TagRegistryNumbersFrom(fs->registry, (uint32_t)pool.top_tag + 1u);
    return MountCensus(what);
}

void MountAfterDoor(void)
{
    uint32_t* fids  = s_last_span;
    uint32_t  count = s_last_span_count;
    s_last_span       = NULL;
    s_last_span_count = 0;
    if (!fids)
        return;

    TagFSState* fs    = tagfs_get_state();
    uint32_t    reset = 0;
    for (uint32_t i = 0; i < count && IntegrityIsInitialized(); i++) {
        FileRecord* rec = FileMapRecordHold(fs->files, fids[i], NULL);
        if (!rec)
            continue;
        for (uint32_t e = 0; e < rec->extent_count; e++)
            IntegrityForget(rec->extents[e].start_block, rec->extents[e].block_count);
        FileRecordRelease(rec);
        reset++;
    }
    kfree(fids);
    if (reset)
        kprintf("[TagFS] the volume was not stopped cleanly after cycle %llu: the digests of "
                "the %u file(s) that cycle wrote records of are set back to unknown — a block "
                "one of them wrote in place after it may have landed without its digest, or "
                "its digest without it, and neither is rot\n",
                (unsigned long long)s_unclean_cycle, reset);
}
