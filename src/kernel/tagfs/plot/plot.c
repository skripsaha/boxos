#include "plot_internal.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../block_map/block_map_derived.h"
#include "pmm.h"

/*
 * The Plot of one call: loaded from the file's record in the map of live
 * files, worked on in memory, inked as one record, and let go of — its Stakes
 * with it when nothing inked them (CR-1). See plot.h.
 */

#define PLOT_BLOCK TAGFS_BLOCK_SIZE

static uint64_t PlotBlocksFor(uint64_t bytes)
{
    return (bytes + PLOT_BLOCK - 1) / PLOT_BLOCK;
}

/* Room for `need` elements of `size` bytes in *array, which holds `have`. */
bool PlotRoom(void **array, uint32_t *room, uint32_t have, uint64_t need, size_t size)
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

error_t PlotLoad(Plot *p, uint64_t fid, InkWarrant warrant)
{
    memset(p, 0, sizeof(*p));
    p->fid         = fid;
    p->mount_epoch = tagfs_mount_epoch();
    p->warrant     = warrant;

    RecordWhere where;
    FileRecord *rec = FileMapRecordHold(tagfs_get_state()->files, fid, &where);
    if (!rec)
        return ERR_FILE_NOT_FOUND;
    p->shape = where.shape;
    p->book  = tagfs_get_state()->format == DEED_FORMAT_V2;
    if (FileRecordCopyOut(rec, &p->meta) != 0) {
        FileRecordRelease(rec);
        return ERR_NO_MEMORY;
    }
    if (p->book) {
        /* A book's Plot holds the record and works on a window of it, never
         * a copy of every run (plot_window.c). */
        PlotWindowLoad(p, rec);
    } else {
        /* The record's header is the Plot's own copy, and its runs laid flat
         * are the Plot's runs from here on — the one copy of them a call of
         * format v1 works on and packs (PackV1); the record itself is never
         * changed, and the Ink conveys what the Plot did to it
         * (PlotConveyance). */
        error_t flat = FileRecordFlatten(rec, &p->runs, &p->run_count);
        FileRecordRelease(rec);
        if (flat != OK)
            return ERR_NO_MEMORY;
        p->run_room = p->run_count;
        for (uint32_t i = 0; i < p->run_count; i++)
            p->blocks += p->runs[i].count;
        p->blocks_before = p->blocks;
        p->runs_before   = PackV1Extents(p->runs, p->run_count);
    }
    p->size_before   = p->meta.size;
    p->size          = p->meta.size;

    /* A record whose blocks do not reach its size is not a file to write
     * over: the write would lay a fresh file over bytes it cannot see. */
    if (PlotBlocksFor(p->size) > p->blocks) {
        kprintf("[TagFS] file %llu: its record says %llu bytes and names %llu block(s), fewer "
                "than that needs — nothing is written over it\n", fid,
                (unsigned long long)p->size, (unsigned long long)p->blocks);
        return ERR_METADATA_CORRUPTED;
    }
    /* A file the system keeps, to a call with no warrant; the kernel the
     * loader reads by its blocks, or a volume whose medium left, to every
     * call: refused here, before a byte of the call moves. A book's file is
     * known for the kernel's by its id (ink.c). */
    return InkMayChange(fid, p->meta.tag_ids, p->meta.tag_count, p->runs, p->run_count,
                        warrant);
}

int PlotGiveBack(const Plot *p, uint64_t start, uint32_t count)
{
    if (p->book)
        return -(int)BlockMapDerivedGive(p->fid, start, count);
    return tagfs_free_blocks(start, count);
}

void PlotPullStakes(Plot *p)
{
    if (p->stake_count && p->mount_epoch != tagfs_mount_epoch()) {
        kprintf("[TagFS] file %llu: the volume was mounted again while %u run(s) were staked "
                "for it — they are not given back to a mounting they were never taken from\n",
                p->fid, p->stake_count);
        p->stake_count = 0;
        return;
    }
    for (uint32_t i = 0; i < p->stake_count; i++) {
        int given = PlotGiveBack(p, p->stakes[i].start, p->stakes[i].count);
        if (given != 0)
            kprintf("[TagFS] file %llu: %u block(s) at %llu were taken for it and named by no "
                    "record, and would not go back (error %d)\n", p->fid,
                    p->stakes[i].count, (unsigned long long)p->stakes[i].start, -given);
    }
    p->stake_count = 0;
}

void PlotRelease(Plot *p)
{
    PlotPullStakes(p);
    InkRoomLetGo(p);
    PlotWindowLetGo(p);
    kfree(p->runs);
    kfree(p->stakes);
    kfree(p->freed);
    tagfs_metadata_free(&p->meta);
    memset(p, 0, sizeof(*p));
}

bool PlotFind(const Strip *runs, uint32_t count, uint64_t k,
              uint64_t *out_block, uint32_t *out_left)
{
    uint64_t base = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t n = runs[i].count;
        if (k < base + n) {
            uint32_t at = (uint32_t)(k - base);
            *out_block = runs[i].start + at;
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

uint64_t PlotStretchBytes(const PlotStretch *s, uint32_t blocks)
{
    if (blocks == 0)
        return 0;
    uint64_t carried = (uint64_t)blocks * PLOT_BLOCK - s->head;
    return carried < s->bytes ? carried : s->bytes;
}

bool PlotStretchAt(const Plot *p, uint64_t pos, uint64_t left, uint32_t most, PlotStretch *out)
{
    if (p->book)
        return PlotWindowStretchAt(p, pos, left, most, out);
    uint64_t k     = pos / PLOT_BLOCK;
    uint64_t block = 0;
    uint32_t after = 0;
    if (left == 0 || most == 0 || !PlotFind(p->runs, p->run_count, k, &block, &after))
        return false;

    uint32_t head  = (uint32_t)(pos % PLOT_BLOCK);
    uint64_t count = PlotBlocksFor(head + left);
    if (count > (uint64_t)after + 1)
        count = (uint64_t)after + 1;
    if (count > most)
        count = most;

    uint64_t carried = count * PLOT_BLOCK - head;
    out->block = block;
    out->count = (uint32_t)count;
    out->head  = head;
    out->bytes = carried < left ? carried : left;

    uint64_t in_first = PLOT_BLOCK - head < out->bytes ? PLOT_BLOCK - head : out->bytes;
    out->first = PlotFillFor(p, k, head, (uint32_t)in_first);
    out->last  = out->first;
    if (count > 1)
        out->last = PlotFillFor(p, k + count - 1, 0,
                                (uint32_t)(head + out->bytes - (count - 1) * PLOT_BLOCK));
    out->from_first = block;
    out->from_last  = block + count - 1;
    out->k          = k;
    return true;
}

void PlotRunsPut(Strip *runs, uint32_t *n, uint64_t start, uint32_t count)
{
    if (count == 0)
        return;
    if (*n > 0) {
        Strip *last = &runs[*n - 1];
        if (last->kind == STRIP_DATA && last->start + last->count == start &&
            (uint64_t)last->count + count <= UINT32_MAX) {
            last->count += count;
            return;
        }
    }
    runs[*n] = (Strip){ .start = start, .count = count, .kind = STRIP_DATA };
    (*n)++;
}

error_t PlotAppendRun(Plot *p, uint64_t start, uint32_t count)
{
    if (count == 0)
        return OK;
    if (!PlotRoom((void **)&p->runs, &p->run_room, p->run_count,
                  (uint64_t)p->run_count + 1, sizeof(Strip)))
        return ERR_NO_MEMORY;
    /* On a book a run the record names is never grown: the stitch would say
     * its ground was taken out and put back, and a reader frees what it takes
     * out (spec_v2 §2.8). Only this call's own Stake is. */
    const Strip *last = p->run_count ? &p->runs[p->run_count - 1] : NULL;
    if (p->book && last && PlotStakeHolding(p, last->start) == p->stake_count)
        p->runs[p->run_count++] = (Strip){ .start = start, .count = count, .kind = STRIP_DATA };
    else
        PlotRunsPut(p->runs, &p->run_count, start, count);
    p->blocks += count;
    return OK;
}

error_t PlotStake(Plot *p, uint64_t start, uint32_t count)
{
    if (!PlotRoom((void **)&p->stakes, &p->stake_room, p->stake_count,
                  (uint64_t)p->stake_count + 1, sizeof(Stake)))
        return ERR_NO_MEMORY;
    p->stakes[p->stake_count].start = start;
    p->stakes[p->stake_count].count = count;
    p->stake_count++;
    return OK;
}

error_t PlotLetGoLater(Plot *p, uint64_t start, uint32_t count)
{
    if (count == 0)
        return OK;
    if (!PlotRoom((void **)&p->freed, &p->freed_room, p->freed_count,
                  (uint64_t)p->freed_count + 1, sizeof(Strip)))
        return ERR_NO_MEMORY;
    p->freed[p->freed_count++] = (Strip){ .start = start, .count = count, .kind = STRIP_DATA };
    return OK;
}

/* The Stake holding `block`, or stake_count when no Stake does. */
uint32_t PlotStakeHolding(const Plot *p, uint64_t block)
{
    for (uint32_t i = 0; i < p->stake_count; i++)
        if (block >= p->stakes[i].start && block - p->stakes[i].start < p->stakes[i].count)
            return i;
    return p->stake_count;
}

/*
 * [start, start + count) is no longer the file's. What of it this call staked
 * goes back now — no record ever named it; what a record named comes to rest
 * in Fallow once a record that no longer names it is inked.
 */
error_t PlotLetGo(Plot *p, uint64_t start, uint32_t count)
{
    while (count > 0) {
        uint32_t i = PlotStakeHolding(p, start);
        if (i == p->stake_count) {
            /* Named ground, up to where the next Stake begins. */
            uint32_t n = count;
            for (uint32_t s = 0; s < p->stake_count; s++)
                if (p->stakes[s].start > start && p->stakes[s].start - start < n)
                    n = (uint32_t)(p->stakes[s].start - start);
            error_t rc = PlotLetGoLater(p, start, n);
            if (rc != OK)
                return rc;
            start += n;
            count -= n;
            continue;
        }

        Stake    *s   = &p->stakes[i];
        uint64_t  end = s->start + s->count;
        uint32_t  n   = (uint32_t)(end - start < count ? end - start : count);
        if (start != s->start && start + n != end) {
            /* Out of its middle: the Stake becomes two. */
            uint64_t tail_start = start + n;
            uint32_t tail_count = (uint32_t)(end - tail_start);
            error_t  rc         = PlotStake(p, tail_start, tail_count);
            if (rc != OK)
                return rc;
            s = &p->stakes[i];
            s->count = (uint32_t)(start - s->start);
        } else if (start == s->start) {
            s->start += n;
            s->count -= n;
        } else {
            s->count -= n;
        }
        if (s->count == 0)
            p->stakes[i] = p->stakes[--p->stake_count];

        int given = PlotGiveBack(p, start, n);
        if (given != 0)
            kprintf("[TagFS] file %llu: %u block(s) at %llu it took and no longer needs would not "
                    "go back (error %d)\n", p->fid, n, (unsigned long long)start, -given);
        start += n;
        count -= n;
    }
    return OK;
}

error_t PlotTrimTo(Plot *p, uint64_t keep_blocks)
{
    if (p->book && p->blocks > keep_blocks) {
        error_t rc = PlotWindowTrim(p, keep_blocks);
        if (rc != OK)
            return rc;
    }
    while (p->blocks > keep_blocks && p->run_count > 0) {
        Strip   *last = &p->runs[p->run_count - 1];
        uint64_t over = p->blocks - keep_blocks;
        uint32_t cut  = last->count < over ? last->count : (uint32_t)over;
        /* A hole owns no ground — its start names no block — so a cut into
         * one lets nothing go (plot_window.c, PlotRestake, passes it over
         * alike). */
        error_t  rc   = last->kind == STRIP_DATA ? PlotLetGo(p, last->start + last->count - cut, cut) : OK;
        if (rc != OK)
            return rc;
        last = &p->runs[p->run_count - 1];
        last->count -= cut;
        p->blocks   -= cut;
        if (last->count == 0)
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

bool PlotRecordFits(const Plot *p)
{
    TagFSMetadata record = PlotRecord(p);
    return PlotFits(&record, NULL);
}

/* The record fits, or its runs are gathered until it does, or the write is
 * refused — said, with the numbers. A gather's own refusal is the answer as it
 * gave it: a run that waits for a Settle cycle (ERR_WOULD_BLOCK) is not a
 * record full for good, nor a medium that refused the copy. */
static error_t PlotFitOrGather(Plot *p)
{
    if (PlotRecordFits(p))
        return OK;

    uint32_t room    = PlotRunRoom(&p->meta);
    uint64_t extents = PackV1Extents(p->runs, p->run_count);
    if (extents <= room) {
        kprintf("[TagFS] file %llu: its %u tag(s) and its name leave its record no room it "
                "needs — refused\n", p->fid, p->meta.tag_count);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint64_t hint = p->run_count ? p->runs[0].start : 0;
    error_t  rc   = PlotGather(p, extents - room, hint);
    if (rc == OK && PlotRecordFits(p))
        return OK;
    if (rc == OK)
        rc = ERR_TAGFS_RECORD_FULL;
    if (rc != ERR_WOULD_BLOCK)
        kprintf("[TagFS] file %llu: growing it would leave it %llu extent(s) and its record holds "
                "%u — refused (error %u), and nothing of it written\n", p->fid,
                (unsigned long long)extents, room, (unsigned)rc);
    return rc;
}

error_t PlotReserve(Plot *p, uint64_t end)
{
    uint64_t need = PlotBlocksFor(end);
    uint32_t room = PlotRunRoom(&p->meta);

    while (p->blocks < need) {
        /* The new ground alone already takes more extents than the record
         * holds: gathering moves only what the record named, so no gather
         * could make room, and nothing more is taken. */
        if (PackV1Extents(p->runs, p->run_count) > (uint64_t)room + p->runs_before) {
            kprintf("[TagFS] file %llu: the free ground it would grow into lies in more runs "
                    "than its record holds extents (%u) — refused, and nothing of it "
                    "written\n", p->fid, room);
            return ERR_TAGFS_RECORD_FULL;
        }

        uint64_t left = need - p->blocks;
        uint32_t want = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        uint64_t hint = 0;
        if (p->run_count) {
            const Strip *last = &p->runs[p->run_count - 1];
            hint = last->start + last->count;
        }

        /* ERR_WOULD_BLOCK is room on its way, not a refusal: its caller waits
         * for the cycle that brings it and asks again. */
        uint64_t start = 0;
        uint32_t got   = 0;
        error_t  rc    = AllotmentTake(p->fid, want, p->blocks, hint, &start, &got);
        if (rc != OK) {
            if (rc != ERR_WOULD_BLOCK)
                kprintf("[TagFS] file %llu: %llu more block(s) could not be had for it (error %u) "
                        "— the write is refused\n", p->fid, (unsigned long long)left,
                        (unsigned)rc);
            return rc;
        }
        rc = PlotStake(p, start, got);
        if (rc != OK) {
            (void)PlotGiveBack(p, start, got);
            return rc;
        }
        rc = PlotAppendRun(p, start, got);
        if (rc != OK)
            return rc;
    }
    error_t rc = PlotFitOrGather(p);
    return rc == OK ? InkRoomClaim(p) : rc;
}

error_t PlotBeginWrite(Plot *p, uint64_t offset, bool append, uint64_t bytes, uint64_t *out_at)
{
    uint64_t at = append ? p->size : offset;
    if (at > p->size)
        return ERR_TAGFS_BEYOND_SIZE;
    if (bytes > UINT64_MAX - at)
        return ERR_INVALID_ARGUMENT;
    error_t rc = p->book ? PlotWindowBegin(p, at, bytes) : PlotReserve(p, at + bytes);
    if (rc != OK)
        return rc;
    *out_at = at;
    return OK;
}

error_t PlotEndWrite(Plot *p, uint64_t at, uint64_t done)
{
    if (p->book) {
        error_t rc = PlotWindowEnd(p, at, done);
        if (rc != OK)
            return rc;
    }
    uint64_t end  = at + done;
    uint64_t keep = PlotBlocksFor(end);
    if (keep < p->blocks_before)
        keep = p->blocks_before;
    if (end > p->size)
        p->size = end;
    return PlotTrimTo(p, keep);
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
