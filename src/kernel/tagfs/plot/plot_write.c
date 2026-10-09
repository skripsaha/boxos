#include "plot_internal.h"
#include "scribe.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "../run/run.h"
#include "../ink/fallow.h"                 /* FallowBlocks — ground a cycle gives back */
#include "../glean/glean.h"                /* GleanMayFree — ground the pool's slack gives back */
#include "../metadata_pool/meta_pool.h"

/*
 * Blocks written the synchronous way, a stretch a command (PlotStretchAt):
 * the part of the write that lies in one run on the medium, as much of it as
 * the seat takes at once (step 1.11). Each block by the one read-modify-write
 * rule (PlotFillFor), which only the first and the last block of a stretch
 * can need: a block the call took is zeros under what is written and is never
 * read — it holds a previous owner's bytes, not the file's; a block the file
 * had is read first, and a read the medium refuses stops the write there
 * rather than laying zeros over the file's own bytes (S1), as does a block
 * that does not match its digest, whose rot a fresh digest would make the
 * file's for good (RT-22); a block the write covers whole is neither, and is
 * never refused for what it held. A stop inside a stretch still lays the
 * blocks before it, in the one command they fill.
 *
 * The stretch is staged in a buffer of its own from the heap: a kernel stack
 * has no room for even one block.
 */

uint8_t *PlotRunBuffer(uint32_t *most)
{
    uint8_t *buf = NULL;
    while (*most && !(buf = kmalloc((size_t)*most * TAGFS_BLOCK_SIZE)))
        *most /= 2u;
    return buf;
}

/* Block i of the stretch staged in `into`: the rest of a first or last block
 * read or zeroed by the rule, and the write's bytes laid over it. OK, or why
 * the write stops at this block. */
static error_t PlotStage(Plot *p, const PlotStretch *s, uint32_t i, const PlotSource *src,
                         uint64_t from, uint8_t *into)
{
    uint32_t off   = i == 0 ? s->head : 0;
    uint64_t at    = PlotStretchBytes(s, i);
    uint32_t chunk = (uint32_t)(PlotStretchBytes(s, i + 1) - at);
    PlotFill fill  = i == 0 ? s->first : (i == s->count - 1 ? s->last : PLOT_FILL_WHOLE);

    if (fill == PLOT_FILL_FRESH) {
        memset(into, 0, TAGFS_BLOCK_SIZE);
    } else if (fill == PLOT_FILL_READ) {
        error_t rc = tagfs_read_block(s->block + i, into);
        if (rc != OK) {
            kprintf("[TagFS] file %llu: block %llu answered %u to its read, so the %u byte(s) meant "
                    "for it are not laid over the rest of it — the write stops there\n",
                    p->fid, (unsigned long long)(s->block + i), (unsigned)rc, chunk);
            return rc;
        }
    }

    error_t rc = PlotSourcePull(src, from + at, into + off, chunk);
    if (rc != OK)
        kprintf("[TagFS] file %llu: the bytes to write from %llu on could not be taken from the "
                "pages they are in (error %u) — the write stops there\n", p->fid,
                (unsigned long long)(from + at), (unsigned)rc);
    return rc;
}

/* One stretch, staged in `buf` block by block and laid in one command: the
 * blocks before the first one that stopped it, or all of them. *out_laid: the
 * write's bytes that landed. */
static error_t PlotLayStretch(Plot *p, const PlotStretch *s, const PlotSource *src,
                              uint64_t from, uint8_t *buf, uint64_t *out_laid)
{
    uint32_t staged = 0;
    error_t  stop   = OK;
    while (staged < s->count &&
           (stop = PlotStage(p, s, staged, src, from,
                             buf + (size_t)staged * TAGFS_BLOCK_SIZE)) == OK)
        staged++;

    uint32_t landed = 0;
    error_t  rc     = OK;
    if (staged > 0) {
        /* The door's first question, asked before it: a medium already gone
         * is sent no command, and none is counted. One that leaves under the
         * command refuses it with the same answer, and that one went out. */
        bool     sent  = !TagFSMediumGone();
        uint64_t began = rdtsc();
        rc = TagFSWriteRun(s->block, staged, buf, &landed);
        p->data_us += cpu_tsc_to_us(rdtsc() - began);
        if (sent)
            TagFSDataCostLaid(staged);
        if (rc != OK)
            kprintf("[TagFS] file %llu: blocks %llu..%llu would not be written (error %u) — %u of "
                    "them had landed before the medium stopped answering\n", p->fid,
                    (unsigned long long)s->block, (unsigned long long)(s->block + staged - 1),
                    (unsigned)rc, landed);
    }
    *out_laid = PlotStretchBytes(s, landed);
    return rc != OK ? rc : stop;
}

error_t PlotWriteBlocks(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done)
{
    uint32_t most = TagFSRunBlocks();
    uint8_t *buf  = PlotRunBuffer(&most);
    uint64_t done = 0;
    error_t  rc   = OK;

    *out_done = 0;
    if (!buf)
        return ERR_NO_MEMORY;

    while (done < src->bytes && rc == OK) {
        PlotStretch s;
        if (!PlotStretchAt(p, at + done, src->bytes - done, most, &s)) {
            kprintf("[TagFS] file %llu: no extent of its plot holds block %llu, which the write "
                    "reserved — the write stops at byte %llu\n", p->fid,
                    (unsigned long long)((at + done) / TAGFS_BLOCK_SIZE),
                    (unsigned long long)(at + done));
            rc = ERR_TAGFS_EXTENT_ERROR;
            break;
        }
        uint64_t laid = 0;
        rc = PlotLayStretch(p, &s, src, done, buf, &laid);
        done += laid;
    }
    if (rc != OK)
        kprintf("[TagFS] file %llu: %llu of %llu byte(s) of the write landed\n", p->fid,
                (unsigned long long)done, (unsigned long long)src->bytes);

    kfree(buf);
    /* What landed is the medium's only once a flush issued after it is kept:
     * the cycle that names these blocks issues it before the name. */
    if (done > 0)
        p->data_need = VolumeFlushNeed();
    *out_done = done;
    return rc;
}

/* [k, end) of the file's blocks written from `buf`, a run a command: at most
 * `most` blocks each, block k's own bytes staged in buf's first block by the
 * caller and every other block of buf zeros. */
static error_t PlotLayZeros(Plot *p, uint64_t k, uint64_t end, uint8_t *buf, uint32_t most,
                            uint32_t kept)
{
    while (k < end) {
        uint64_t block = 0;
        uint32_t after = 0;
        if (!PlotFind(p->runs, p->run_count, k, &block, &after))
            return ERR_TAGFS_EXTENT_ERROR;
        uint64_t n = end - k;
        if (n > (uint64_t)after + 1)
            n = (uint64_t)after + 1;
        if (n > most)
            n = most;

        uint32_t landed = 0;
        error_t  rc     = TagFSWriteRun(block, (uint32_t)n, buf, &landed);
        if (rc != OK) {
            kprintf("[TagFS] file %llu: blocks %llu..%llu would not take their zeros (error %u), "
                    "so the file is not grown\n", p->fid, (unsigned long long)block,
                    (unsigned long long)(block + n - 1), (unsigned)rc);
            return rc;
        }
        memset(buf, 0, kept);   /* the old last block's bytes went out with the first run */
        kept = 0;
        k   += n;
    }
    return OK;
}

error_t PlotExtendZero(Plot *p, uint64_t size)
{
    if (size <= p->size)
        return OK;

    uint64_t from = p->size;
    error_t  rc   = PlotReserve(p, size);
    if (rc != OK)
        return rc;

    uint32_t most = TagFSRunBlocks();
    uint8_t *buf  = PlotRunBuffer(&most);
    if (!buf)
        return ERR_NO_MEMORY;
    memset(buf, 0, (size_t)most * TAGFS_BLOCK_SIZE);

    uint64_t k    = from / TAGFS_BLOCK_SIZE;
    uint64_t end  = (size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    uint32_t tail = (uint32_t)(from % TAGFS_BLOCK_SIZE);

    /* The old last block keeps the file's bytes up to the old size and has,
     * past it, whatever the block held before — a cut tail, a previous owner.
     * It is read, zeroed from the old size on, and goes out with the zeros
     * after it. */
    if (tail) {
        uint64_t block = 0;
        if (!PlotFind(p->runs, p->run_count, k, &block, NULL)) {
            rc = ERR_TAGFS_EXTENT_ERROR;
            goto out;
        }
        rc = tagfs_read_block(block, buf);
        if (rc != OK) {
            kprintf("[TagFS] file %llu: its last block %llu answered %u to its read, so it is not "
                    "grown past its %llu bytes\n", p->fid, (unsigned long long)block,
                    (unsigned)rc, (unsigned long long)from);
            goto out;
        }
        memset(buf + tail, 0, TAGFS_BLOCK_SIZE - tail);
    }

    rc = PlotLayZeros(p, k, end, buf, most, tail);
    if (rc == OK) {
        p->size      = size;
        p->data_need = VolumeFlushNeed();
    }
out:
    kfree(buf);
    return rc;
}

error_t PlotRoomFor(const Plot *p, uint64_t end)
{
    uint64_t need = end / TAGFS_BLOCK_SIZE + (end % TAGFS_BLOCK_SIZE != 0);
    if (need <= p->blocks)
        return OK;
    need -= p->blocks;

    TagFSState *fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint64_t free    = fs->ledger.free_blocks;
    uint64_t resting = FallowBlocks();
    uint64_t slack   = GleanMayFree() ? PoolChainBlocks() - PoolLiveBlocks() - 1u : 0;
    TagFSLockDrop(&fs->lock);

    if (need <= free + resting + slack)
        return OK;
    kprintf("[TagFS] file %llu: growing it to %llu bytes needs %llu more block(s), and the volume "
            "could give it %llu at most — %llu free, %llu resting in Fallow, %llu in the pool's "
            "slack — refused, the file %llu bytes long\n", p->fid, (unsigned long long)end,
            (unsigned long long)need, (unsigned long long)(free + resting + slack),
            (unsigned long long)free, (unsigned long long)resting, (unsigned long long)slack,
            (unsigned long long)p->size);
    return ERR_TAGFS_NO_SPACE;
}
