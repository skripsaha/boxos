#include "perambulation.h"
#include "../ink/docket.h"
#include "../integrity/gloss.h"
#include "../box_hash/box_hash.h"
#include "../file_map/file_record.h"
#include "boardroom.h"
#include "kheap.h"
#include "tagfs_volume.h"

/*
 * AwayProof and AwayProofPlot (perambulation.h): the data no stitch settled,
 * read back off a medium that left and came back and held to the digests it
 * was written with — the Gloss's for an Ink's blocks, the Plot's own for a
 * write not inked yet (plot_window.c, PlotWindowLanded). The data of a file's
 * change is the ground its record put in since the last stitch naming it: a
 * Born's whole ground, a Shaped's inserted Strips (FileRecordSplices) — a
 * block the Gloss does not name is ground no write of this mount laid, and is
 * not read.
 */

#define AWAY_PROOF_RUN 16u     /* blocks a read takes at once */

typedef struct {
    uint8_t        seat;
    uint64_t       base;
    uint8_t*       window;     /* AWAY_PROOF_RUN blocks */
    BoxHashContext ctx;
    uint64_t       proved;
    uint64_t       missed;
} AwayProofRead;

/* Blocks [start, start + count) held: each the Gloss names read back and held
 * to its digest — a block the medium cannot read held to nothing. OK, or the
 * medium's refusal. */
static error_t AwayProofRun(TagFSVolume* vol, AwayProofRead* a, uint64_t start, uint64_t count)
{
    for (uint64_t k = 0; k < count;) {
        uint32_t n  = count - k < AWAY_PROOF_RUN ? (uint32_t)(count - k) : AWAY_PROOF_RUN;
        int      rc = BoardroomRead(a->seat, a->base + (start + k) * TAGFS_BLOCK_SECTORS, n * TAGFS_BLOCK_SECTORS,
                                    a->window);
        if (rc != 0 && rc != (int)ERR_BAD_SECTOR)
            return ERR_READ_FAILED;
        for (uint32_t b = 0; b < n; b++) {
            uint64_t want = 0, block = start + k + b;
            if (!GlossFind(vol, block, &want))
                continue;
            int one = rc == 0 ? 0
                              : BoardroomRead(a->seat, a->base + block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS,
                                              a->window + (uint64_t)b * TAGFS_BLOCK_SIZE);
            if (one != 0 && one != (int)ERR_BAD_SECTOR)
                return ERR_READ_FAILED;
            a->proved++;
            if (one != 0 ||
                BoxHashIntegrity(a->window + (uint64_t)b * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, &a->ctx) != want)
                a->missed++;
        }
        k += n;
    }
    return OK;
}

/* The ground file `now` holds that `was` did not — all of it for a file born
 * since the last stitch — held. OK, ERR_NO_MEMORY, or the medium's refusal. */
static error_t AwayProofJot(TagFSVolume* vol, AwayProofRead* a, const FileRecord* was, const FileRecord* now)
{
    error_t rc = OK;
    if (!was) {
        StripWalk w;
        Strip     s;
        StripWalkBegin(now, &w);
        while (rc == OK && StripWalkNextGround(&w, &s))
            if (s.kind == STRIP_DATA)       /* a run SHARED names ground no stitch since wrote */
                rc = AwayProofRun(vol, a, s.start, s.count);
        return rc;
    }
    BookSplice* sp     = NULL;
    Strip*      strips = NULL;
    uint64_t    count  = 0, inserted = 0;
    rc = FileRecordSplices(was, now, &sp, &count, &strips);
    for (uint64_t i = 0; rc == OK && i < count; i++)
        inserted += sp[i].ninsert;
    for (uint64_t i = 0; rc == OK && i < inserted; i++)
        if (strips[i].kind == STRIP_DATA)
            rc = AwayProofRun(vol, a, strips[i].start, strips[i].count);
    kfree(sp);
    kfree(strips);
    return rc;
}

error_t AwayProof(TagFSVolume* vol, uint8_t seat, uint64_t base, uint64_t* proved, uint64_t* missed)
{
    TagFSState*   fs  = &vol->state;
    AwayProofRead a   = { .seat = seat, .base = base };
    uint64_t      fid = 0;
    error_t       rc  = OK;
    *proved = 0;
    *missed = 0;
    if (!(a.window = kmalloc((uint64_t)AWAY_PROOF_RUN * TAGFS_BLOCK_SIZE)))
        return ERR_NO_MEMORY;
    BoxHashInit(&a.ctx, fs->uuid, 16);
    for (;;) {
        FileRecord *was = NULL, *now = NULL;
        uint64_t    at  = 0;
        TagFSLockTake(&fs->lock);
        bool any = DocketWasFrom(vol, fid, &at, &was, &now);
        TagFSLockDrop(&fs->lock);
        if (!any)
            break;
        if (now)
            rc = AwayProofJot(vol, &a, was, now);
        FileRecordRelease(was);
        FileRecordRelease(now);
        if (rc != OK || at == UINT64_MAX)
            break;
        fid = at + 1u;
    }
    kfree(a.window);
    *proved = a.proved;
    *missed = a.missed;
    return rc;
}

error_t AwayProofPlot(TagFSVolume* vol, const Plot* p, uint64_t* departed)
{
    BoxHashContext ctx;
    uint64_t       missed = 0;
    *departed = AwayDepartures(vol);
    if (!p->digest_count || p->departed == *departed)
        return OK;
    uint8_t* block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!block)
        return ERR_NO_MEMORY;
    BoxHashInit(&ctx, vol->state.uuid, 16);
    for (uint32_t i = 0; i < p->digest_count; i++) {
        int rc = tagfs_volume_read(vol, p->digests[i].block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, block);
        if (rc != 0 && rc != (int)ERR_BAD_SECTOR) {
            kfree(block);
            return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY : ERR_READ_FAILED;
        }
        if (rc != 0 || BoxHashIntegrity(block, TAGFS_BLOCK_SIZE, &ctx) != p->digests[i].digest)
            missed++;
    }
    kfree(block);
    if (!missed)
        return OK;
    kprintf("[TagFS] file %llu: %llu of the %u block(s) its write laid before its medium left and came back do not "
            "read back as written — the write is refused (ERR_TAGFS_NOT_LANDED)\n", (unsigned long long)p->fid,
            (unsigned long long)missed, p->digest_count);
    return ERR_TAGFS_NOT_LANDED;
}
