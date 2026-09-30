#include "plot_internal.h"
#include "scribe.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * Blocks written the synchronous way, each by the one read-modify-write rule
 * (PlotFillFor): a block the call took is zeros under what is written and is
 * never read — it holds a previous owner's bytes, not the file's; a block the
 * file had is read first, and a read the medium refuses stops the write there
 * rather than laying zeros over the file's own bytes (S1); a block the write
 * covers whole is neither.
 *
 * The block moves through a buffer of its own from the heap: a kernel stack
 * has no room for 4 KiB.
 */

/* Where block k lies, walking forward: the run the last block came from while
 * it lasts, a search when it ends. */
static bool PlotNextBlock(const Plot *p, uint64_t k, bool first, uint32_t *block, uint32_t *left)
{
    if (!first && *left > 0) {
        (*block)++;
        (*left)--;
        return true;
    }
    return PlotFind(p->runs, p->run_count, k, block, left);
}

error_t PlotWriteBlocks(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done)
{
    uint8_t *buf  = kmalloc(TAGFS_BLOCK_SIZE);
    uint64_t done = 0;
    uint32_t block = 0, left = 0;
    error_t  rc   = OK;

    *out_done = 0;
    if (!buf)
        return ERR_NO_MEMORY;

    while (done < src->bytes) {
        uint64_t pos    = at + done;
        uint64_t k      = pos / TAGFS_BLOCK_SIZE;
        uint32_t offset = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
        uint64_t rest   = src->bytes - done;
        uint32_t chunk  = TAGFS_BLOCK_SIZE - offset;
        if (chunk > rest)
            chunk = (uint32_t)rest;

        if (!PlotNextBlock(p, k, done == 0, &block, &left)) {
            kprintf("[TagFS] file %u: no extent of its plot holds block %llu, which the write "
                    "reserved — the write stops at byte %llu\n", p->fid,
                    (unsigned long long)k, (unsigned long long)pos);
            rc = ERR_TAGFS_EXTENT_ERROR;
            break;
        }

        PlotFill fill = PlotFillFor(p, k, offset, chunk);
        if (fill == PLOT_FILL_FRESH) {
            memset(buf, 0, TAGFS_BLOCK_SIZE);
        } else if (fill == PLOT_FILL_READ && (rc = tagfs_read_block(block, buf)) != OK) {
            kprintf("[TagFS] file %u: block %u would not read, so the %u byte(s) meant for it "
                    "are not laid over the rest of it — the write stops at byte %llu, %llu of "
                    "%llu written\n", p->fid, block, chunk, (unsigned long long)pos,
                    (unsigned long long)done, (unsigned long long)src->bytes);
            break;
        }

        rc = PlotSourcePull(src, done, buf + offset, chunk);
        if (rc != OK) {
            kprintf("[TagFS] file %u: the bytes to write from %llu on could not be taken from "
                    "the pages they are in (error %u) — %llu of %llu written\n", p->fid,
                    (unsigned long long)done, (unsigned)rc, (unsigned long long)done,
                    (unsigned long long)src->bytes);
            break;
        }

        uint64_t began = rdtsc();
        rc = tagfs_write_block(block, buf);
        p->data_us += cpu_tsc_to_us(rdtsc() - began);
        if (rc != OK) {
            kprintf("[TagFS] file %u: block %u would not be written — %llu of %llu bytes had "
                    "landed before the medium stopped answering\n", p->fid, block,
                    (unsigned long long)done, (unsigned long long)src->bytes);
            break;
        }
        done += chunk;
    }

    kfree(buf);
    /* What landed is the medium's only once a flush issued after it is kept:
     * the cycle that names these blocks issues it before the name. */
    if (done > 0)
        p->data_need = VolumeFlushNeed();
    *out_done = done;
    return rc;
}

error_t PlotExtendZero(Plot *p, uint64_t size)
{
    if (size <= p->size)
        return OK;

    uint64_t from = p->size;
    error_t  rc   = PlotReserve(p, size);
    if (rc != OK)
        return rc;

    uint8_t *buf = kmalloc(TAGFS_BLOCK_SIZE);
    if (!buf)
        return ERR_NO_MEMORY;

    uint64_t k     = from / TAGFS_BLOCK_SIZE;
    uint64_t end   = (size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    uint32_t block = 0, left = 0;
    bool     first = true;

    /* The old last block keeps the file's bytes up to the old size and has,
     * past it, whatever the block held before — a cut tail, a previous owner.
     * It is read, zeroed from the old size on, and written back. */
    uint32_t tail = (uint32_t)(from % TAGFS_BLOCK_SIZE);
    if (tail) {
        if (!PlotNextBlock(p, k, first, &block, &left)) {
            rc = ERR_TAGFS_EXTENT_ERROR;
            goto out;
        }
        first = false;
        rc = tagfs_read_block(block, buf);
        if (rc != OK) {
            kprintf("[TagFS] file %u: its last block %u would not read, so it is not grown "
                    "past its %llu bytes\n", p->fid, block, (unsigned long long)from);
            goto out;
        }
        memset(buf + tail, 0, TAGFS_BLOCK_SIZE - tail);
        rc = tagfs_write_block(block, buf);
        if (rc != OK)
            goto out;
        k++;
    }

    memset(buf, 0, TAGFS_BLOCK_SIZE);
    for (; k < end; k++) {
        if (!PlotNextBlock(p, k, first, &block, &left)) {
            rc = ERR_TAGFS_EXTENT_ERROR;
            goto out;
        }
        first = false;
        rc = tagfs_write_block(block, buf);
        if (rc != OK) {
            kprintf("[TagFS] file %u: block %u would not take its zeros, so the file is not "
                    "grown to %llu bytes\n", p->fid, block, (unsigned long long)size);
            goto out;
        }
    }
    p->size      = size;
    p->data_need = VolumeFlushNeed();
out:
    kfree(buf);
    return rc;
}
