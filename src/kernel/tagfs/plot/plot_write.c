#include "plot_internal.h"
#include "scribe.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "../run/run.h"
#include "../file_map/tail_slip.h"
#include "../mount/away.h"                 /* AwayDepartures — what a book's write is held to at its Ink */

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
        uint64_t old = i == 0 ? s->from_first : s->from_last;
        error_t  rc  = tagfs_read_block(old, into);
        if (rc != OK) {
            kprintf("[TagFS] file %llu: block %llu answered %u to its read, so the %u byte(s) meant "
                    "for it are not laid over the rest of it — the write stops there\n",
                    p->fid, (unsigned long long)old, (unsigned)rc, chunk);
            return rc;
        }
    }
    /* The rest of the file's last block is what a read of it gives: its base
     * and the Slips over it (tail_slip.h) — the block the write lays carries
     * them, and the record that names it lets them go. */
    if (fill != PLOT_FILL_WHOLE)
        (void)TailSlipOver(p->rec, s->k + i, into);

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
        bool     sent     = !TagFSMediumGone();
        uint64_t departed = AwayDepartures();
        uint64_t began    = rdtsc();
        rc = TagFSWriteRun(s->block, staged, buf, &landed);
        p->data_us += cpu_tsc_to_us(rdtsc() - began);
        if (sent)
            TagFSDataCostLaid(staged);
        error_t noted = PlotWindowLanded(p, departed, s->block, buf, landed);
        if (rc == OK && noted != OK) {
            landed = 0;      /* a block whose digest is not held is no block of the file's */
            rc     = noted;
        }
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
    *out_done = 0;
    if (p->slip)
        return PlotSlipWrite(p, at, src, out_done);   /* its bytes ride the stitch: no block moves */

    uint32_t most = TagFSRunBlocks();
    uint8_t *buf  = PlotRunBuffer(&most);
    uint64_t done = 0;
    error_t  rc   = OK;

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

error_t PlotExtendZero(Plot *p, uint64_t size)
{
    if (size <= p->size)
        return OK;
    return PlotWindowGrow(p, size);
}

error_t PlotRoomFor(const Plot *p, uint64_t end)
{
    return PlotWindowRoomFor(p, end);
}
