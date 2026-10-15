#include "mount.h"
#include "away.h"
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
#include "../glean/glean.h"
#include "../scribe/scribe.h"

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

/* Whether the seal this mount stood on was written by a machine stopping
 * cleanly, and its cycle (MountStopWasClean). */
static bool      s_stop_clean;
static uint64_t  s_stop_cycle;

bool MountStopWasClean(uint64_t* out_cycle)
{
    if (out_cycle)
        *out_cycle = s_stop_cycle;
    return s_stop_clean;
}

/* A block the Deed names → the block TagFS allocates in. The Deed states where
 * the registry, the file table, the pool and the BOOT run are in blocks of the
 * whole volume — what makes a volume readable by something that has not
 * mounted it; TagFS counts from the start of the data run, as every extent on
 * the medium is counted. This is the seam. */
static uint32_t MountDataBlock(uint32_t volume_block)
{
    return volume_block - TagFSLayout()->data_block;
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
    bool     torn;           /* sealed, its open block rolled back: torn_block up to torn_end */
    uint32_t torn_block;
    uint32_t torn_end;
    uint32_t issued_below;   /* sealed: the stood Ledger's next file id; 0, a count let wrap */
    uint32_t checked;
    uint32_t corrected;      /* a row naming another record than the newest */
    uint32_t derived;        /* sealed: a row the table lacked, set from the pool */
    uint32_t unsettled;      /* sealed: a row of an id the stood Ledger had not issued, no record */
    uint32_t lost;           /* a row with no record that reads: no seal, or a file that settled */
    uint32_t unnamed;        /* no seal: a record no row names */
    uint64_t top_fid;
} MountFileCount;

/*
 * A row with no record left, judged: true when it names a file lost — said,
 * and the name let go with it — false for a row of a cycle that did not
 * settle.
 *
 * ‼ UNDER A SEAL, AN ID ITS LEDGER HAD ISSUED NAMES A FILE THAT SETTLED. An id
 * is issued and the file's first record laid in one hold of the state lock
 * (InkNew), and a cycle's S0 takes the Ledger's image under that lock: every
 * id below the stood Ledger's next one had its first record in the seal's span
 * — and each record after it lies further on, so the span held one of them.
 * None left there is a file lost — rolled back with the open block, or gone
 * with a block the chain no longer holds — and its loss is the person's, said
 * as one, never counted with the cycle that did not settle: that cycle's rows
 * name ids the Ledger had not issued yet. A row naming the rolled-back open
 * block short of the bytes the seal said it held is said as such.
 */
static bool MountRowLost(uint64_t fid, uint32_t mb, uint32_t mo, const MountFileCount* n)
{
    bool rolled = n->torn && mb == n->torn_block && mo < n->torn_end;
    bool issued = n->issued_below == 0 || fid < n->issued_below;
    if (n->sealed && !issued)
        return false;
    /* Said with kprintf, so it survives into a shipped build: this line was
     * the silence that turned a lost file into a mystery. */
    kprintf("[TagFS FSCK] file %llu is named at block %u+%u and this volume's pool holds no "
            "record of it that reads%s — the file is gone and its name goes with it\n",
            fid, mb, mo, rolled ? ", its only one settled in the open block the mount "
            "rolled back" : n->sealed ? ", though the seal it stands on had settled it" : "");
    return true;
}

/* Out of line: the walk's path down the record's Tract would stand on the
 * mount's own frame, which the batch of ids it walks by nearly fills. */
__attribute__((noinline)) void MountClaimRuns(const FileRecord* rec, uint64_t fid)
{
    StripWalk walk;
    Strip     run;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run))
        GroundClaim(GROUND_FILE, run.start, run.count, fid);
}

/*
 * One file the map holds — a row, a record, or both — judged by the rules the
 * volume stands on, and put into the tag index and the census. OK, or
 * ERR_NO_MEMORY — said — when the tag index has no memory for it.
 *
 * ‼ WHERE THE POOL SAYS THE NEWEST RECORD IS, WINS. The pool is only appended
 * to and the walk read it in order, so the record the map holds is the newest
 * one of the file the walk reached. A row naming somewhere else is corrected
 * to it, in memory now and on the medium by the next cycle.
 */
static error_t MountFile(uint64_t fid, MountFileCount* n)
{
    TagFSState* fs = tagfs_get_state();
    RecordWhere where;
    uint32_t    mb = 0, mo = 0;
    FileRecord* rec     = FileMapRecordHold(fs->files, fid, &where);
    bool        has_row = file_table_lookup(fid, &mb, &mo) == 0;

    if (fid > n->top_fid)
        n->top_fid = fid;

    if (!rec) {
        if (MountRowLost(fid, mb, mo, n))
            n->lost++;
        else
            n->unsettled++;
        file_table_delete(fid);
        PoolMirrorDrop(fid);
        return OK;
    }
    if (!has_row && !n->sealed) {
        n->unnamed++;
        FileRecordRelease(rec);
        PoolMirrorDrop(fid);
        return OK;
    }

    uint32_t pb = 0;
    if (PoolLinkBlock(where.link, &pb) && (!has_row || pb != mb || where.offset != mo)) {
        file_table_update(fid, pb, where.offset);
        if (has_row)
            n->corrected++;
        else
            n->derived++;
    }
    MountClaimRuns(rec, fid);

    /* A file its tags do not find is a file lost to every query until the
     * next mount, and a trashed one shows as live: the mount refuses whole,
     * as the registry's and the pool's do when memory runs out. */
    for (uint32_t t = 0; t < rec->tag_count; t++) {
        if (tag_bitmap_set(fs->bitmap_index, rec->tags[t], fid) != 0) {
            kprintf("[TagFS] there is no memory to put file %llu under tag %u in the tag index — "
                    "%u of this volume's files were indexed before it, and a file its tags do "
                    "not find is lost to every query, so the volume is not mounted\n",
                    fid, rec->tags[t], n->checked);
            FileRecordRelease(rec);
            return ERR_NO_MEMORY;
        }
    }
    n->checked++;
    FileRecordRelease(rec);
    return OK;
}

/* Every file the map holds, a batch of ids at a time: no lock is held while
 * one is judged, which may take it out of the map. OK, or the first refusal. */
static error_t MountWalkFiles(MountFileCount* n)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    batch[64];
    uint64_t    from = 1;
    uint32_t    got;
    while ((got = FileMapFiles(fs->files, from, batch, ARRAY_SIZE(batch))) > 0) {
        for (uint32_t i = 0; i < got; i++) {
            error_t rc = MountFile(batch[i], n);
            if (rc != OK)
                return rc;
        }
        if (batch[got - 1] == UINT64_MAX)
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
    return OK;
}

/*
 * ‼ A QUITCLAIM TAKES ITS ROW WITH IT, ONCE THERE IS A ROW TO TAKE.
 *
 * The walk puts a Quitclaim through the mirror's door like every Ink, and the
 * door lets the file's row go — but the pool is walked before the table is
 * read, so at the walk there was no row, and the table then brought it back:
 * a file deleted for good was judged a row with no record, "lost" with no
 * seal and a cycle that did not settle under one. The rows of the files the
 * walk left taken out are let go here, once the table is read, and said; the
 * next cycle writes their blocks of the table without them.
 */
static void MountQuitclaimedRows(const PoolMountOutcome* pool)
{
    TagFSState* fs  = tagfs_get_state();
    uint32_t    let = 0;
    for (uint32_t i = 0; i < pool->gone_count; i++) {
        uint32_t    fid = pool->gone[i];
        uint32_t    mb, mo;
        FileRecord* rec = FileMapRecordHold(fs->files, fid, NULL);
        if (rec) {
            FileRecordRelease(rec);     /* a newer record put it back */
            continue;
        }
        if (file_table_lookup(fid, &mb, &mo) != 0)
            continue;
        file_table_delete(fid);
        let++;
    }
    if (let)
        kprintf("[TagFS] %u table row(s) named files whose newest record is a Quitclaim — "
                "deleted, their rows not yet gone from the table — and are let go with them\n",
                let);
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
 * furthest. An id past 0xFFFFFFFE is every id issued (LedgerIssueFileId): the
 * counter is format v1's, a u32, whatever width memory holds the ids in.
 */
uint32_t MountNextFileId(uint32_t ledger_next, uint64_t top_row, uint64_t top_record)
{
    uint64_t top  = top_row > top_record ? top_row : top_record;
    uint64_t next = top + 1u;
    if (next > UINT32_MAX)
        next = UINT32_MAX;
    return ledger_next == 0 || next <= ledger_next ? ledger_next : (uint32_t)next;
}

/*
 * And past every id this kernel gave out on the volume before a book of it was
 * given up (away.h): the programs that hold them live on, and the medium never
 * saw them. A count an earlier kernel let wrap stays the medium's.
 */
static void MountFidBoundary(uint64_t top_row, uint64_t top_record, uint64_t known)
{
    VolumeLedger* led  = &tagfs_get_state()->ledger;
    uint32_t      next = MountNextFileId(led->next_file_id, top_row, top_record);
    if (next != led->next_file_id) {
        kprintf("[TagFS] the Ledger's next file id is %u and the medium names file id %llu — "
                "the ids from %u on were seen by a cycle the Ledger did not count, so the next "
                "file is given %u, and none of them again\n", led->next_file_id,
                (unsigned long long)(top_row > top_record ? top_row : top_record),
                led->next_file_id, next);
        led->next_file_id = next;
    }
    if (led->next_file_id != 0 && known > led->next_file_id) {
        kprintf("[TagFS] this kernel gave file ids up to %llu out on this volume before its book "
                "was given up, and the programs holding them live on — the next file is given "
                "%llu, and none of them again\n", (unsigned long long)(known - 1u),
                (unsigned long long)known);
        led->next_file_id = (uint32_t)known;   /* format v1: kept from this Ledger's own count */
    }
}

/* The tag ids likewise: past every one a record of the pool wears, and every
 * one this kernel gave out before a book of the volume was given up. */
static void MountTagBoundary(uint32_t top_tag, uint32_t known)
{
    TagFSState* fs   = tagfs_get_state();
    uint32_t    next = top_tag + 1u;
    TagFSLockTake(&fs->registry->lock);
    uint32_t    held = fs->registry->next_id;
    TagFSLockDrop(&fs->registry->lock);
    if (known > next && known > held)
        kprintf("[TagFS] this kernel gave tag ids up to %u out on this volume before its book "
                "was given up — none of them is given again\n", known - 1u);
    TagRegistryNumbersFrom(fs->registry, known > next ? known : next);
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
    if (boot->kernel_blocks && boot->kernel_block >= TagFSLayout()->data_block)
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
    PoolClaimGround(MountDataBlock(TagFSLayout()->metadata_pool_block));
    GleanClaimGround();
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
        *what = "there is no memory to build its tag registry";
        return ERR_NO_MEMORY;
    }
    /* A registry that would not read is not an empty registry: every tag id
     * on this volume is defined there, so a mount that continues without it
     * renames nothing and mislabels everything. And a registry the machine had
     * no memory for is not one that would not read: that mount is asked again
     * when the memory comes back. */
#ifdef CONFIG_VOLUME_TESTS
    TagFSStandMountRegistryLoads();
#endif
    error_t rc = tag_registry_load(fs->registry, MountDataBlock(TagFSLayout()->tag_registry_block),
                                   seals->has[0] ? &seals->mark[0] : NULL,
                                   seals->has[1] ? &seals->mark[1] : NULL);
    if (rc == ERR_NO_MEMORY) {
        *what = "there is no memory to hold its tag registry";
        return rc;
    }
    if (rc != OK) {
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
        .head = MountDataBlock(TagFSLayout()->metadata_pool_block),
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
    GleanMount(ask.head, PoolChainFirst());

    bool lied = pool->lied || UseMemoryMediumLied() || AwayLiedLearned(fs->uuid) ||
                (pool->seal >= 0 && (seals->mark[pool->seal].flags & SETTLE_MEDIUM_LIED));
    SettleMountStand(pool->from_block, pool->from_bytes, lied);

    /* RT-22: what the stood seal's own cycle wrote, when that cycle was not
     * a machine stopping — kept for MountAfterDoor. A mount refused after
     * this point left its list behind; this one lets it go. */
    kfree(s_last_span);
    s_last_span       = NULL;
    s_last_span_count = 0;
    s_unclean_cycle   = 0;
    s_stop_clean      = pool->seal >= 0 && (seals->mark[pool->seal].flags & SETTLE_CLEAN);
    s_stop_cycle      = pool->seal >= 0 ? seals->mark[pool->seal].settle : 0;
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
    uint32_t    region  = TagFSLayout()->block_bitmap_blocks * TAGFS_BLOCK_SECTORS;
    uint32_t    sectors = (TagFSLayout()->data_blocks + TAGFS_SECTOR_SIZE * 8 - 1) / (TAGFS_SECTOR_SIZE * 8);
    if (sectors > region) {
        *what = "its block bitmap region is too small to hold a bit for every block of its "
                "data run";
        return ERR_TAGFS_CORRUPTED;
    }
    if (BlockMapMount(TagFSLayout()->data_blocks,
                      (uint64_t)TagFSLayout()->block_bitmap_block * TAGFS_BLOCK_SECTORS) != OK) {
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

    /* A v1 Deed names no Braid assembly: the volume's every block is its own
     * medium's, and no read or write asks Braid (braid/braid.h). */
    fs->braid = NULL;

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

    MountFileCount files = {
        .sealed       = pool.stood != POOL_STOOD_NO_SEAL,
        .torn         = pool.stood == POOL_STOOD_TORN_TAIL,
        .torn_block   = pool.torn_block,
        .torn_end     = pool.torn_end,
        .issued_below = fs->ledger.next_file_id,
    };
    rc = file_table_init(MountDataBlock(TagFSLayout()->file_table_block), files.sealed);
    if (rc == OK)
        MountQuitclaimedRows(&pool);
    kfree(pool.gone);
    pool.gone = NULL;
    if (rc == ERR_NO_MEMORY) {
        *what = "there is no memory to hold its file table";
        return rc;
    }
    if (rc != OK) {
        *what = "its file table would not read";
        return ERR_FILE_TABLE_CORRUPT;
    }

    fs->bitmap_index = tag_bitmap_create();
    if (!fs->bitmap_index) {
        *what = "there is no memory for its tag index";
        return ERR_NO_MEMORY;
    }
    rc = MountBlockMap(what);
    if (rc != OK)
        return rc;

    GroundCensusBegin();
    rc = MountWalkFiles(&files);
    if (rc != OK) {
        GroundCensusEnd();
        *what = "there is no memory for its tag index";
        return rc;
    }
    fs->ledger.total_files = files.checked;

    uint64_t known_file;
    uint32_t known_tag;
    AwayNumbersKnown(fs->uuid, &known_file, &known_tag);
    MountFidBoundary(files.top_fid, pool.top_fid, known_file);
    MountTagBoundary(pool.top_tag, known_tag);
    rc = MountCensus(what);
    if (rc != OK)
        return rc;

    /* Said HERE and not beside the Ledger, because until the bitmap has been
     * read there is nothing to say: the Ledger's own claim is a number the
     * mount is about to check. */
    kprintf("[TagFS] %u data blocks, %llu free, %llu files\n", TagFSLayout()->data_blocks,
            (unsigned long long)fs->ledger.free_blocks, (unsigned long long)fs->ledger.total_files);
    return OK;
}

void MountAfterDoor(void)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V1)
        return;

    // Data-block integrity map (verify-on-read). After initialized=true so a
    // map laid by the cycles can take its run from the block allocator.
    error_t integrity = IntegrityInit();
    if (integrity == OK)
        kprintf("[TagFS] data-integrity verify-on-read active\n");
    else if (integrity != ERR_WOULD_BLOCK)
        kprintf("[TagFS] integrity map unavailable - continuing without verify-on-read\n");

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
        StripWalk   walk;
        Strip       run;
        if (!rec)
            continue;
        StripWalkBegin(rec, &walk);
        while (StripWalkNextGround(&walk, &run))
            IntegrityForget(run.start, run.count);
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

void MountPromise(void)
{
    /*
     * A format-v2 volume (book_mount.h): under glass it is read and nothing of
     * it is written, and promises only that. Otherwise this kernel's writer
     * lays stitches of files, names and memory on its ribbon (settle/sewing.h)
     * — no file's data — and no Tally: the next mount reads the ribbon whole;
     * and a medium that leaves and comes back is read again from its book:
     * what no stitch settled is lost, which is what `anchor` is for (§16.2
     * п.28).
     */
    if (tagfs_get_state()->format == DEED_FORMAT_V2 && TagFSIsUnderGlass()) {
        kprintf("[TagFS] what this volume promises: it is read as its book holds it and nothing of it is "
                "written — a change is refused (ERR_TAGFS_READ_ONLY); a medium that leaves and comes back "
                "is read again from its book\n");
        return;
    }
    if (tagfs_get_state()->format == DEED_FORMAT_V2) {
        kprintf("[TagFS] what this volume promises: its book takes files made, named, tagged and given up, "
                "the names of tags and the memory of its Use Context, a stitch on its ribbon a Settle cycle — a "
                "change is a stitch kept by its cycle's flush within %llu ms, and a name given goes with the next "
                "stitch sewn; an `anchor` is answered once a stitch of this mount carrying what it waits for is "
                "kept; a change to a file's data this kernel refuses (ERR_TAGFS_READ_ONLY); a medium that leaves "
                "and comes back is read again from its book, and what no stitch settled before an `anchor` is "
                "lost\n",
                (unsigned long long)(SETTLE_WINDOW_US / 1000u));
        kprintf("[TagFS] no Tally is written by this kernel: the ribbon is read whole at every mount\n");
        return;
    }

    /*
     * ‼ AND WHAT THAT MOUNT PROMISES, SAID ONCE, IN PLAIN WORDS — the difference
     * between before and after an anchor included (RT-06/07): until then a
     * change lives in the last cycle's open tail, which the next cycle writes
     * again in place, and a flash page torn by the power can take it; an
     * anchored change lies in a closed block nothing writes again. A promise
     * nobody is told is not a promise, and the document is not on the screen
     * when the power goes.
     */
    kprintf("[TagFS] what this volume promises: a file's content reaches the medium as it "
            "is written, and its name, size and extents follow on their own within %llu ms "
            "(a Settle cycle, settle.h); until `anchor` a change stands as the last cycle "
            "left it, but for a flash page torn by the power; after `anchor`, for good\n",
            (unsigned long long)(SETTLE_WINDOW_US / 1000u));

    /* The Spares a delete stands on, laid by now so a volume with no free
     * block left can still give a file up (meta_pool.h, SPARE_RESERVE). A
     * volume with no room for them says so, and deletes while its tail lasts.
     * Their stamps are writes, and a write no flush kept leaves the book of a
     * volume nobody has written to in doubt (mount/away.h): one flush keeps
     * them, and every write the mount made before them. */
    error_t spared = PoolSpareRefill(SPARE_RESERVE);
    if (spared != OK)
        kprintf("[TagFS] the Spares a delete stands on could not all be laid (error %u) — "
                "a full volume deletes only while its pool's tail has room\n",
                (unsigned)spared);
    error_t kept = VolumeFlushFor(VolumeFlushNeed());
    if (kept != OK)
        kprintf("[TagFS] the flush after the mount's own writes was refused (error %u) — "
                "until a Settle cycle's flush is kept, a medium that leaves and comes "
                "back is not taken up as it was left\n", (unsigned)kept);
}
