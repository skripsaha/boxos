#include "plot_internal.h"
#include "../metadata_pool/meta_pool.h"
#include "pmm.h"

/*
 * The Plot of one call: loaded from the record the file table names, worked on
 * in memory, committed as one record, and let go of — its Stakes with it when
 * nothing committed them (CR-1). See plot.h.
 */

#define PLOT_BLOCK TAGFS_BLOCK_SIZE

static uint64_t PlotBlocksFor(uint64_t bytes)
{
    return (bytes + PLOT_BLOCK - 1) / PLOT_BLOCK;
}

/* Room for `need` elements of `size` bytes in *array, which holds `have`. */
static bool PlotRoom(void **array, uint32_t *room, uint32_t have, uint64_t need, size_t size)
{
    if (need <= *room)
        return true;
    if (need > UINT32_MAX)
        return false;
    uint64_t cap = *room ? *room : 8;
    while (cap < need)
        cap *= 2;
    if (cap > UINT32_MAX)
        cap = need;
    void *grown = kmalloc((size_t)cap * size);
    if (!grown)
        return false;
    if (*array && have)
        memcpy(grown, *array, (size_t)have * size);
    kfree(*array);
    *array = grown;
    *room  = (uint32_t)cap;
    return true;
}

error_t PlotLoad(Plot *p, uint32_t fid)
{
    memset(p, 0, sizeof(*p));
    p->fid         = fid;
    p->mount_epoch = tagfs_mount_epoch();
    if (file_table_lookup(fid, &p->meta_block, &p->meta_offset) != 0)
        return ERR_FILE_NOT_FOUND;
    int read = meta_pool_read(p->meta_block, p->meta_offset, &p->meta);
    if (read != 0) {
        kprintf("[TagFS] file %u: its record, block %u at %u, does not read (error %d) — "
                "nothing is written over a file whose record cannot be read\n",
                fid, p->meta_block, p->meta_offset, -read);
        return (error_t)-read;
    }

    /* The record's extents are the Plot's runs from here on. */
    p->runs      = p->meta.extents;
    p->run_count = p->meta.extent_count;
    p->run_room  = p->meta.extent_count;
    p->meta.extents      = NULL;
    p->meta.extent_count = 0;
    for (uint32_t i = 0; i < p->run_count; i++)
        p->blocks += p->runs[i].block_count;
    p->blocks_before = p->blocks;
    p->runs_before   = p->run_count;
    p->size_before   = p->meta.size;
    p->size          = p->meta.size;

    /* A record whose blocks do not reach its size is not a file to write
     * over: the write would lay a fresh file over bytes it cannot see. */
    if (PlotBlocksFor(p->size) > p->blocks) {
        kprintf("[TagFS] file %u: its record says %llu bytes and names %llu block(s), fewer "
                "than that needs — nothing is written over it\n", fid,
                (unsigned long long)p->size, (unsigned long long)p->blocks);
        return ERR_METADATA_CORRUPTED;
    }
    return OK;
}

void PlotPullStakes(Plot *p)
{
    if (p->stake_count && p->mount_epoch != tagfs_mount_epoch()) {
        kprintf("[TagFS] file %u: the volume was mounted again while %u run(s) were staked "
                "for it — they are not given back to a mounting they were never taken from\n",
                p->fid, p->stake_count);
        p->stake_count = 0;
        return;
    }
    for (uint32_t i = 0; i < p->stake_count; i++) {
        int given = tagfs_free_blocks(p->stakes[i].start, p->stakes[i].count);
        if (given != 0)
            kprintf("[TagFS] file %u: %u block(s) at %u were taken for it and named by no "
                    "record, and would not go back (error %d)\n", p->fid,
                    p->stakes[i].count, p->stakes[i].start, -given);
    }
    p->stake_count = 0;
}

void PlotRelease(Plot *p)
{
    PlotPullStakes(p);
    kfree(p->runs);
    kfree(p->stakes);
    kfree(p->freed);
    tagfs_metadata_free(&p->meta);
    memset(p, 0, sizeof(*p));
}

bool PlotFind(const FileExtent *runs, uint32_t count, uint64_t k,
              uint32_t *out_block, uint32_t *out_left)
{
    uint64_t base = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t n = runs[i].block_count;
        if (k < base + n) {
            uint32_t at = (uint32_t)(k - base);
            *out_block = runs[i].start_block + at;
            if (out_left)
                *out_left = (uint32_t)(n - at - 1);
            return true;
        }
        base += n;
    }
    return false;
}

bool PlotExisted(const Plot *p, uint64_t k)
{
    return k < PlotBlocksFor(p->size_before);
}

PlotFill PlotFillFor(const Plot *p, uint64_t k, uint32_t offset, uint32_t bytes)
{
    if (offset == 0 && bytes == PLOT_BLOCK)
        return PLOT_FILL_WHOLE;
    return PlotExisted(p, k) ? PLOT_FILL_READ : PLOT_FILL_FRESH;
}

/* How many more extents [start, start + count) can take when appended. */
static uint64_t PlotPiecesFor(uint32_t count)
{
    return (uint64_t)count / PLOT_RUN_MOST + 2;
}

void PlotRunsPut(FileExtent *runs, uint32_t *n, uint32_t start, uint32_t count)
{
    while (count > 0) {
        if (*n > 0) {
            FileExtent *last = &runs[*n - 1];
            if ((uint64_t)last->start_block + last->block_count == start &&
                last->block_count < PLOT_RUN_MOST) {
                uint32_t add = PLOT_RUN_MOST - last->block_count;
                if (add > count)
                    add = count;
                last->block_count = (uint16_t)(last->block_count + add);
                start += add;
                count -= add;
                continue;
            }
        }
        uint32_t take = count < PLOT_RUN_MOST ? count : PLOT_RUN_MOST;
        runs[*n].start_block = start;
        runs[*n].block_count = (uint16_t)take;
        (*n)++;
        start += take;
        count -= take;
    }
}

error_t PlotAppendRun(Plot *p, uint32_t start, uint32_t count)
{
    if (count == 0)
        return OK;
    if (!PlotRoom((void **)&p->runs, &p->run_room, p->run_count,
                  (uint64_t)p->run_count + PlotPiecesFor(count), sizeof(FileExtent)))
        return ERR_NO_MEMORY;
    PlotRunsPut(p->runs, &p->run_count, start, count);
    p->blocks += count;
    return OK;
}

error_t PlotStake(Plot *p, uint32_t start, uint32_t count)
{
    if (!PlotRoom((void **)&p->stakes, &p->stake_room, p->stake_count,
                  (uint64_t)p->stake_count + 1, sizeof(Stake)))
        return ERR_NO_MEMORY;
    p->stakes[p->stake_count].start = start;
    p->stakes[p->stake_count].count = count;
    p->stake_count++;
    return OK;
}

error_t PlotLetGoLater(Plot *p, uint32_t start, uint32_t count)
{
    while (count > 0) {
        uint32_t n = count < PLOT_RUN_MOST ? count : PLOT_RUN_MOST;
        if (!PlotRoom((void **)&p->freed, &p->freed_room, p->freed_count,
                      (uint64_t)p->freed_count + 1, sizeof(FileExtent)))
            return ERR_NO_MEMORY;
        p->freed[p->freed_count].start_block = start;
        p->freed[p->freed_count].block_count = (uint16_t)n;
        p->freed_count++;
        start += n;
        count -= n;
    }
    return OK;
}

/* The Stake holding `block`, or stake_count when no Stake does. */
static uint32_t PlotStakeHolding(const Plot *p, uint32_t block)
{
    for (uint32_t i = 0; i < p->stake_count; i++)
        if (block >= p->stakes[i].start && block - p->stakes[i].start < p->stakes[i].count)
            return i;
    return p->stake_count;
}

/*
 * [start, start + count) is no longer the file's. What of it this call staked
 * goes back now — no record ever named it; what a record named goes back once
 * a record that no longer names it is committed.
 */
static error_t PlotLetGo(Plot *p, uint32_t start, uint32_t count)
{
    while (count > 0) {
        uint32_t i = PlotStakeHolding(p, start);
        if (i == p->stake_count) {
            /* Named ground, up to where the next Stake begins. */
            uint32_t n = count;
            for (uint32_t s = 0; s < p->stake_count; s++)
                if (p->stakes[s].start > start && p->stakes[s].start - start < n)
                    n = p->stakes[s].start - start;
            error_t rc = PlotLetGoLater(p, start, n);
            if (rc != OK)
                return rc;
            start += n;
            count -= n;
            continue;
        }

        Stake    *s   = &p->stakes[i];
        uint64_t  end = (uint64_t)s->start + s->count;
        uint32_t  n   = (uint32_t)(end - start < count ? end - start : count);
        if (start != s->start && start + (uint64_t)n != end) {
            /* Out of its middle: the Stake becomes two. */
            uint32_t tail_start = start + n;
            uint32_t tail_count = (uint32_t)(end - tail_start);
            error_t  rc         = PlotStake(p, tail_start, tail_count);
            if (rc != OK)
                return rc;
            s = &p->stakes[i];
            s->count = start - s->start;
        } else if (start == s->start) {
            s->start += n;
            s->count -= n;
        } else {
            s->count -= n;
        }
        if (s->count == 0)
            p->stakes[i] = p->stakes[--p->stake_count];

        int given = tagfs_free_blocks(start, n);
        if (given != 0)
            kprintf("[TagFS] file %u: %u block(s) at %u it took and no longer needs would not "
                    "go back (error %d)\n", p->fid, n, start, -given);
        start += n;
        count -= n;
    }
    return OK;
}

error_t PlotTrimTo(Plot *p, uint64_t keep_blocks)
{
    while (p->blocks > keep_blocks && p->run_count > 0) {
        FileExtent *last = &p->runs[p->run_count - 1];
        uint64_t    over = p->blocks - keep_blocks;
        uint32_t    cut  = last->block_count < over ? last->block_count : (uint32_t)over;
        uint32_t    from = last->start_block + last->block_count - cut;
        error_t     rc   = PlotLetGo(p, from, cut);
        if (rc != OK)
            return rc;
        last = &p->runs[p->run_count - 1];
        last->block_count = (uint16_t)(last->block_count - cut);
        p->blocks -= cut;
        if (last->block_count == 0)
            p->run_count--;
    }
    return OK;
}

/* The record the Plot would commit, as a view: its own name and tags, the
 * Plot's runs and size. Nothing in it is the view's to free. */
static TagFSMetadata PlotRecord(const Plot *p)
{
    TagFSMetadata record = p->meta;
    record.size         = p->size;
    record.extents      = p->runs;
    record.extent_count = p->run_count;
    return record;
}

/* How many extents the record has room for beside its tags and name. */
uint32_t PlotRunRoom(const Plot *p)
{
    uint64_t fixed = (uint64_t)RECORD_HEADER_SIZE + (uint64_t)p->meta.tag_count * sizeof(uint16_t)
                   + (p->meta.filename ? strlen(p->meta.filename) : 0);
    if (fixed >= TAGFS_MPOOL_DATA_SIZE)
        return 0;
    return (uint32_t)((TAGFS_MPOOL_DATA_SIZE - fixed) / sizeof(FileExtent));
}

bool PlotRecordFits(const Plot *p)
{
    TagFSMetadata record = PlotRecord(p);
    return PlotFits(&record, NULL);
}

/* The record fits, or its extents are gathered until it does, or the write is
 * refused — said, with the numbers. */
static error_t PlotFitOrGather(Plot *p)
{
    if (PlotRecordFits(p))
        return OK;

    uint32_t room = PlotRunRoom(p);
    if (p->run_count <= room) {
        kprintf("[TagFS] file %u: its %u tag(s) and its name leave its record no room it "
                "needs — refused\n", p->fid, p->meta.tag_count);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint32_t runs   = p->run_count;
    uint32_t hint   = p->run_count ? p->runs[0].start_block : 0;
    error_t  rc     = PlotGather(p, runs - room, hint);
    if (rc == OK && PlotRecordFits(p))
        return OK;
    kprintf("[TagFS] file %u: growing it would leave it %u extent(s) and its record holds %u "
            "— refused, and nothing of it written\n", p->fid, runs, room);
    return ERR_TAGFS_RECORD_FULL;
}

error_t PlotReserve(Plot *p, uint64_t end)
{
    uint64_t need = PlotBlocksFor(end);
    uint32_t room = PlotRunRoom(p);

    while (p->blocks < need) {
        /* The new ground alone already takes more extents than the record
         * holds: gathering moves only what the record named, so no gather
         * could make room, and nothing more is taken. */
        if ((uint64_t)p->run_count > (uint64_t)room + p->runs_before) {
            kprintf("[TagFS] file %u: the free ground it would grow into lies in more runs "
                    "than its record holds extents (%u) — refused, and nothing of it "
                    "written\n", p->fid, room);
            return ERR_TAGFS_RECORD_FULL;
        }

        uint64_t left = need - p->blocks;
        uint32_t want = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        uint32_t hint = 0;
        if (p->run_count) {
            const FileExtent *last = &p->runs[p->run_count - 1];
            hint = last->start_block + last->block_count;
        }

        uint32_t start = 0, got = 0;
        error_t  rc    = AllotmentTake(p->fid, want, p->blocks, hint, &start, &got);
        if (rc != OK) {
            kprintf("[TagFS] file %u: %llu more block(s) could not be had for it (error %u) — "
                    "the write is refused\n", p->fid, (unsigned long long)left, (unsigned)rc);
            return rc;
        }
        rc = PlotStake(p, start, got);
        if (rc != OK) {
            (void)tagfs_free_blocks(start, got);
            return rc;
        }
        rc = PlotAppendRun(p, start, got);
        if (rc != OK)
            return rc;
    }
    return PlotFitOrGather(p);
}

error_t PlotBeginWrite(Plot *p, uint64_t offset, bool append, uint64_t bytes, uint64_t *out_at)
{
    uint64_t at = append ? p->size : offset;
    if (at > p->size)
        return ERR_TAGFS_BEYOND_SIZE;
    if (bytes > UINT64_MAX - at)
        return ERR_INVALID_ARGUMENT;
    error_t rc = PlotReserve(p, at + bytes);
    if (rc != OK)
        return rc;
    *out_at = at;
    return OK;
}

error_t PlotEndWrite(Plot *p, uint64_t at, uint64_t done)
{
    uint64_t end  = at + done;
    uint64_t keep = PlotBlocksFor(end);
    if (keep < p->blocks_before)
        keep = p->blocks_before;
    if (end > p->size)
        p->size = end;
    error_t rc = PlotTrimTo(p, keep);
    return rc == OK ? PlotCommit(p) : rc;
}

error_t PlotCommit(Plot *p)
{
    TagFSMetadata record = PlotRecord(p);
    uint32_t      block = 0, offset = 0;

    if (meta_pool_write(&record, &block, &offset) != 0) {
        kprintf("[TagFS] file %u: the pool would not take its record — the file is left as "
                "it was\n", p->fid);
        return ERR_TAGFS_METADATA_ERROR;
    }
    if (file_table_update(p->fid, block, offset) != 0) {
        /* Nothing names the new record: it is retired, and the old one stands. */
        (void)meta_pool_delete(block, offset);
        kprintf("[TagFS] file %u: the file table would not name its new record — the file "
                "is left as it was\n", p->fid);
        return ERR_TAGFS_METADATA_ERROR;
    }
    /* The record it replaces is retired. Should the pool not take the retire,
     * the replaced record still loses to this one: a mount keeps a file's
     * newest record in the pool's own order. */
    (void)meta_pool_delete(p->meta_block, p->meta_offset);

    p->meta_block    = block;
    p->meta_offset   = offset;
    p->stake_count   = 0;
    p->size_before   = p->size;
    p->blocks_before = p->blocks;
    p->runs_before   = p->run_count;

    /* Ground the committed record no longer names goes straight back. Fallow
     * (step 1.4) will hold it until the record that let it go has settled. */
    for (uint32_t i = 0; i < p->freed_count; i++) {
        int given = tagfs_free_blocks(p->freed[i].start_block, p->freed[i].block_count);
        if (given != 0)
            kprintf("[TagFS] file %u: %u block(s) at %u its record no longer names would not "
                    "go back (error %d)\n", p->fid, p->freed[i].block_count,
                    p->freed[i].start_block, -given);
    }
    p->freed_count = 0;
    return OK;
}

error_t PlotSourceBringIn(const PlotSource *src)
{
    if (src->kernel || src->bytes == 0)
        return OK;
    if (!src->vmm || src->bytes > UINTPTR_MAX - src->user)
        return ERR_INVALID_ADDRESS;

    /* One byte of every page, through the copy that brings a page in. */
    uintptr_t end = src->user + src->bytes;
    for (uintptr_t at = src->user; at < end;
         at = (at | (PMM_PAGE_SIZE - 1)) + 1) {
        uint8_t probe;
        error_t rc = vmm_user_buf_in_into(src->vmm, at, 1, &probe);
        if (rc != OK)
            return rc;
    }
    return OK;
}

error_t PlotSourcePull(const PlotSource *src, uint64_t at, void *dst, uint32_t bytes)
{
    if (at > src->bytes || bytes > src->bytes - at)
        return ERR_INVALID_ARGUMENT;
    if (src->kernel) {
        memcpy(dst, src->kernel + at, bytes);
        return OK;
    }
    if (!src->vmm)
        return ERR_INVALID_ADDRESS;

    /* Page by page, each a window onto a page the program has; the walk takes
     * no lock and brings nothing in. */
    uint8_t  *out = dst;
    uintptr_t va  = src->user + (uintptr_t)at;
    while (bytes > 0) {
        uint32_t n = PMM_PAGE_SIZE - (uint32_t)(va & (PMM_PAGE_SIZE - 1));
        if (n > bytes)
            n = bytes;
        const void *page = vmm_translate_user_addr(src->vmm, va, n);
        if (!page)
            return ERR_INVALID_ADDRESS;
        memcpy(out, page, n);
        out   += n;
        va    += n;
        bytes -= n;
    }
    return OK;
}
