#include "braid.h"
#include "boardroom.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/timer/rtc.h"

/* Which of a braid's disks answer, taken under its lock and used without it:
 * a block moves with no Braid lock held. */
typedef struct {
    uint8_t   count;
    uint8_t   members;
    BraidMode mode;
    bool      member[BRAID_MAX_DISKS];
    bool      online[BRAID_MAX_DISKS];
} BraidView;

static void BraidLook(BraidState *b, BraidView *v)
{
    TagFSLockTake(&b->lock);
    v->count   = b->disk_count;
    v->mode    = b->mode;
    v->members = 0;
    for (uint8_t i = 0; i < BRAID_MAX_DISKS; i++) {
        v->member[i] = i < b->disk_count && b->disks[i].member;
        v->online[i] = v->member[i] && b->disks[i].online;
        if (v->member[i])
            v->members++;
    }
    TagFSLockDrop(&b->lock);
}

/* The one disk a stripe keeps block_num on: the (block_num mod members)-th
 * member, so an index no member holds is never a block's home. */
static uint8_t BraidStripeDisk(const BraidView *v, uint64_t block_num)
{
    uint8_t nth = (uint8_t)(block_num % v->members);
    for (uint8_t i = 0; i < v->count; i++) {
        if (v->member[i] && nth-- == 0)
            return i;
    }
    return 0;
}

/* What one command on disk i came to, counted under the lock. */
static void BraidCountRead(BraidState *b, uint8_t i, bool ok, bool tagged)
{
    TagFSLockTake(&b->lock);
    if (ok) {
        b->disks[i].read_count++;
        b->stats.total_reads++;
        if (tagged)
            b->stats.tag_assignments++;
    } else {
        b->disks[i].error_count++;
    }
    TagFSLockDrop(&b->lock);
}

static void BraidCountWrite(BraidState *b, uint8_t i, bool ok, bool tagged)
{
    TagFSLockTake(&b->lock);
    if (ok) {
        b->disks[i].write_count++;
        b->disks[i].used_blocks++;
        b->stats.total_writes++;
        if (tagged)
            b->stats.tag_assignments++;
    } else {
        b->disks[i].error_count++;
    }
    TagFSLockDrop(&b->lock);
}

/* A Braid disk is a Boardroom seat: one number that means the same thing
 * everywhere, where an AHCI port and an ATA drive index used to be two. */
static bool BraidRawRead(uint8_t disk_id, uint64_t sector, void *data)
{
    return BoardroomRead(disk_id, sector, BRAID_BLOCK_SIZE / BOARDROOM_SECTOR_BYTES, data) == 0;
}

static bool BraidRawWrite(uint8_t disk_id, uint64_t sector, const void *data)
{
    return BoardroomWrite(disk_id, sector, BRAID_BLOCK_SIZE / BOARDROOM_SECTOR_BYTES, data) == 0;
}

// Compute tag-based disk assignment (unique to Braid!)
static uint8_t BraidComputeTagDisk(BraidState *b, const uint8_t *tag_context, uint8_t disk_count)
{
    if (!tag_context || disk_count == 0)
        return 0;
    BoxHash tag_hash = BoxHashContent(tag_context, 16, &b->hash_ctx);
    return tag_hash.bytes[0] % disk_count;
}

static bool BraidVerifyChecksum(BraidState *b, const void *data, const BoxHash *expected)
{
    BoxHash computed = BoxHashContent(data, BRAID_BLOCK_SIZE, &b->hash_ctx);
    return BoxHashEqual(&computed, expected);
}

/* How many copies a write lays in `mode`. */
static uint8_t BraidCopies(BraidMode mode)
{
    switch (mode) {
    case BraidModeMirror: return 2;
    case BraidModeWeave:  return 3;
    case BraidModeStripe: return 1;
    }
    return 1;
}

error_t BraidInit(BraidState *b, BraidMode mode, const uint8_t *seed)
{
    if (!b || !seed)
        return ERR_INVALID_ARGUMENT;
    if (b->initialized)
        return ERR_ALREADY_INITIALIZED;
    if (mode != BraidModeMirror && mode != BraidModeStripe && mode != BraidModeWeave)
        return ERR_BRAID_MODE_INVALID;

    memset(b, 0, sizeof(*b));
    TagFSLockInit(&b->lock, "braid", LOCK_RANK_LEAF);
    BoxHashInit(&b->hash_ctx, seed, 16);
    b->magic       = BRAID_MAGIC;
    b->version     = BRAID_VERSION;
    b->mode        = mode;
    b->initialized = true;
    return OK;
}

void BraidShutdown(BraidState *b)
{
    if (!b || !b->initialized)
        return;
    TagFSLockTake(&b->lock);
    b->initialized = false;
    TagFSLockDrop(&b->lock);
}

error_t BraidAddDisk(BraidState *b, uint8_t disk_id, uint64_t total_blocks)
{
    if (!b || !b->initialized)
        return ERR_NOT_INITIALIZED;
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;

    TagFSLockTake(&b->lock);
    BraidDisk *disk = &b->disks[disk_id];
    if (disk->member) {
        TagFSLockDrop(&b->lock);
        return ERR_ALREADY_INITIALIZED;
    }
    memset(disk, 0, sizeof(*disk));
    disk->disk_id      = disk_id;
    disk->member       = true;
    disk->online       = true;
    disk->total_blocks = total_blocks;
    disk->last_seen    = rtc_get_unix64();
    if (disk_id >= b->disk_count)
        b->disk_count = disk_id + 1;
    b->active_disks++;
    TagFSLockDrop(&b->lock);
    return OK;
}

/* A disk taken out is not a disk that failed: disk_failures counts the
 * failures, and this is an owner's decision. */
error_t BraidRemoveDisk(BraidState *b, uint8_t disk_id)
{
    if (!b || !b->initialized)
        return ERR_NOT_INITIALIZED;
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;

    TagFSLockTake(&b->lock);
    BraidDisk *disk = &b->disks[disk_id];
    if (!disk->member) {
        TagFSLockDrop(&b->lock);
        return ERR_DEVICE_NOT_READY;
    }
    if (disk->online)
        b->active_disks--;
    disk->member = false;
    disk->online = false;
    TagFSLockDrop(&b->lock);
    return OK;
}

/* A member going offline is a disk failure; coming back is not counted. */
error_t BraidSetDiskOnline(BraidState *b, uint8_t disk_id, bool online)
{
    if (!b || !b->initialized)
        return ERR_NOT_INITIALIZED;
    if (disk_id >= BRAID_MAX_DISKS)
        return ERR_OUT_OF_RANGE;

    TagFSLockTake(&b->lock);
    BraidDisk *disk = &b->disks[disk_id];
    if (!disk->member) {
        TagFSLockDrop(&b->lock);
        return ERR_DEVICE_NOT_READY;
    }
    disk->last_seen = rtc_get_unix64();
    if (disk->online != online) {
        disk->online = online;
        if (online) {
            b->active_disks++;
        } else {
            b->active_disks--;
            b->stats.disk_failures++;
        }
    }
    TagFSLockDrop(&b->lock);
    return OK;
}

error_t BraidReadBlock(BraidState *b, uint64_t block_num, void *data, const BoxHash *expected_checksum)
{
    if (!b || !data)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized)
        return ERR_NOT_INITIALIZED;

    BraidView v;
    BraidLook(b, &v);
    if (v.members == 0)
        return ERR_BRAID_INSUFFICIENT_DISKS;

    /* A stripe keeps each block on one disk only: any other holds another
     * block at that sector. */
    uint8_t from = 0, to = v.count;
    if (v.mode == BraidModeStripe) {
        from = BraidStripeDisk(&v, block_num);
        to   = from + 1;
    }

    error_t result = ERR_BRAID_DISK_OFFLINE;
    for (uint8_t i = from; i < to; i++) {
        if (!v.online[i])
            continue;
        if (!BraidRawRead(i, block_num, data)) {
            BraidCountRead(b, i, false, false);
            result = ERR_IO;
            continue;
        }
        if (expected_checksum && !BraidVerifyChecksum(b, data, expected_checksum)) {
            TagFSLockTake(&b->lock);
            b->stats.checksum_errors++;
            b->disks[i].error_count++;
            TagFSLockDrop(&b->lock);
            result = ERR_CORRUPTED;
            continue;
        }
        BraidCountRead(b, i, true, false);
        return OK;
    }
    return result;
}

error_t BraidWriteBlock(BraidState *b, uint64_t block_num, const void *data, const uint8_t *tag_context)
{
    (void)tag_context;   /* placement by tag is BraidWriteBlockTagged's */
    if (!b || !data)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized)
        return ERR_NOT_INITIALIZED;

    BraidView v;
    BraidLook(b, &v);
    if (v.members == 0)
        return ERR_BRAID_INSUFFICIENT_DISKS;

    uint8_t want = BraidCopies(v.mode);
    uint8_t from = 0, to = v.count;
    if (v.mode == BraidModeStripe) {
        from = BraidStripeDisk(&v, block_num);
        to   = from + 1;
    }

    uint8_t done = 0;
    for (uint8_t i = from; i < to && done < want; i++) {
        if (!v.online[i])
            continue;
        bool ok = BraidRawWrite(i, block_num, data);
        BraidCountWrite(b, i, ok, false);
        if (ok)
            done++;
    }
    return done > 0 ? OK : ERR_IO;
}

// Tag-aware read (unique to Braid!)
error_t BraidReadBlockTagged(BraidState *b, uint64_t block_num, void *data, const uint8_t *tag_context)
{
    if (!b || !data)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized)
        return ERR_NOT_INITIALIZED;

    BraidView v;
    BraidLook(b, &v);
    uint8_t preferred = BraidComputeTagDisk(b, tag_context, v.count);

    // Try preferred disk first (tag-aware locality)
    if (v.online[preferred]) {
        bool ok = BraidRawRead(preferred, block_num, data);
        BraidCountRead(b, preferred, ok, true);
        if (ok)
            return OK;
    }
    for (uint8_t i = 0; i < v.count; i++) {
        if (i == preferred || !v.online[i])
            continue;
        bool ok = BraidRawRead(i, block_num, data);
        BraidCountRead(b, i, ok, false);
        if (ok)
            return OK;
    }
    return ERR_IO;
}

// Tag-aware write (unique to Braid!)
error_t BraidWriteBlockTagged(BraidState *b, uint64_t block_num, const void *data, const uint8_t *tag_context)
{
    if (!b || !data || !tag_context)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized)
        return ERR_NOT_INITIALIZED;

    BraidView v;
    BraidLook(b, &v);
    uint8_t primary = BraidComputeTagDisk(b, tag_context, v.count);
    uint8_t want    = BraidCopies(v.mode);
    uint8_t done    = 0;

    if (v.online[primary]) {
        bool ok = BraidRawWrite(primary, block_num, data);
        BraidCountWrite(b, primary, ok, true);
        if (ok)
            done++;
    }
    // For mirror/weave modes, replicate to additional disks
    for (uint8_t i = 0; i < v.count && done < want && v.mode != BraidModeStripe; i++) {
        if (i == primary || !v.online[i])
            continue;
        bool ok = BraidRawWrite(i, block_num, data);
        BraidCountWrite(b, i, ok, false);
        if (ok)
            done++;
    }
    return done > 0 ? OK : ERR_IO;
}

/*
 * Auto-healing from mirror (unique to Braid!): every member's copy read, the
 * one most of them agree on chosen, and written back to every member that
 * held another. The copies live in the heap for the call — eight blocks, too
 * many for a stack — and move with no lock held, as every Braid block does;
 * a copy another write changes meanwhile is that write's, and the heal only
 * rewrites disks that disagreed when it looked.
 */
error_t BraidAutoHeal(BraidState *b, uint64_t block_num)
{
    if (!b)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized)
        return ERR_NOT_INITIALIZED;

    BraidView v;
    BraidLook(b, &v);
    if (v.members == 0)
        return ERR_BRAID_INSUFFICIENT_DISKS;

    uint8_t *copies = kmalloc((size_t)v.count * BRAID_BLOCK_SIZE);
    if (!copies)
        return ERR_NO_MEMORY;

    BoxHash hashes[BRAID_MAX_DISKS];
    bool    readable[BRAID_MAX_DISKS];
    memset(readable, 0, sizeof(readable));
    for (uint8_t i = 0; i < v.count; i++) {
        if (!v.online[i])
            continue;
        uint8_t *copy = copies + (size_t)i * BRAID_BLOCK_SIZE;
        readable[i] = BraidRawRead(i, block_num, copy);
        BraidCountRead(b, i, readable[i], false);
        if (readable[i])
            hashes[i] = BoxHashContent(copy, BRAID_BLOCK_SIZE, &b->hash_ctx);
    }

    // Find the hash that appears most often (majority vote)
    uint8_t best_disk  = BRAID_MAX_DISKS;
    uint8_t best_count = 0;
    for (uint8_t i = 0; i < v.count; i++) {
        if (!readable[i])
            continue;
        uint8_t count = 0;
        for (uint8_t j = 0; j < v.count; j++) {
            if (readable[j] && BoxHashEqual(&hashes[i], &hashes[j]))
                count++;
        }
        if (count > best_count) {
            best_count = count;
            best_disk  = i;
        }
    }
    if (best_disk == BRAID_MAX_DISKS) {
        kfree(copies);
        return ERR_BRAID_HEAL_FAILED;
    }

    const uint8_t *agreed = copies + (size_t)best_disk * BRAID_BLOCK_SIZE;
    uint8_t        healed = 0;
    for (uint8_t i = 0; i < v.count; i++) {
        if (i == best_disk || !v.online[i])
            continue;
        if (readable[i] && BoxHashEqual(&hashes[i], &hashes[best_disk]))
            continue;
        bool ok = BraidRawWrite(i, block_num, agreed);
        BraidCountWrite(b, i, ok, false);
        if (ok)
            healed++;
    }
    kfree(copies);

    if (healed > 0) {
        TagFSLockTake(&b->lock);
        b->stats.auto_heals++;
        TagFSLockDrop(&b->lock);
        kprintf("[Braid] block %llu healed: %u disk(s) given the copy %u of them agreed on\n",
                (unsigned long long)block_num, healed, best_count);
    }
    return OK;
}

error_t BraidGetStats(BraidState *b, BraidStats *stats)
{
    if (!b || !stats)
        return ERR_INVALID_ARGUMENT;
    if (!b->initialized) {
        memset(stats, 0, sizeof(*stats));
        return ERR_NOT_INITIALIZED;
    }
    TagFSLockTake(&b->lock);
    *stats = b->stats;
    TagFSLockDrop(&b->lock);
    return OK;
}

error_t BraidPrintStats(BraidState *b)
{
    BraidStats stats;
    error_t    rc = BraidGetStats(b, &stats);
    if (rc != OK)
        return rc;
    kprintf("[Braid] %llu read(s), %llu write(s), %llu checksum error(s), %llu heal(s), "
            "%llu disk failure(s), %llu placed by tag\n",
            (unsigned long long)stats.total_reads, (unsigned long long)stats.total_writes,
            (unsigned long long)stats.checksum_errors, (unsigned long long)stats.auto_heals,
            (unsigned long long)stats.disk_failures, (unsigned long long)stats.tag_assignments);
    return OK;
}

bool BraidIsHealthy(BraidState *b)
{
    if (!b || !b->initialized)
        return false;
    TagFSLockTake(&b->lock);
    bool healthy = b->active_disks >= BraidCopies(b->mode);
    TagFSLockDrop(&b->lock);
    return healthy;
}

uint8_t BraidGetActiveDiskCount(BraidState *b)
{
    if (!b || !b->initialized)
        return 0;
    TagFSLockTake(&b->lock);
    uint8_t count = b->active_disks;
    TagFSLockDrop(&b->lock);
    return count;
}
