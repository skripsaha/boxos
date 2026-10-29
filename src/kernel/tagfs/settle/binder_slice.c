#include "binder_internal.h"
#include "../file_map/file_map.h"
#include "../ink/docket.h"
#include "../use_memory/use_memory.h"

/*
 * The slice a Tally is written of (binder.c, TALLY_TAKE): every live file's
 * record as the last settled stitch leaves it, a batch under the state lock at
 * a time, and the ground DIGESTS speaks for — memory alone, no read of the
 * medium. Its own file beside binder.c, which keeps the Tally's steps.
 */

// ----------------------------------------------------------------------------
// TALLY_TAKE: the slice
// ----------------------------------------------------------------------------

#define BINDER_SLICE_BATCH  32u     /* files a hold of the state lock looks at */

/* A batch of files from `*from` on, below `to`, into chunk `c`, under the
 * state lock: each as the last settled stitch left it — the Docket's `was`
 * for one changed since (none for one born since), the map's record for the
 * rest. False once there is no file left below `to`. */
static bool SliceBatch(BinderWork* w, BinderFiles* c, uint64_t* from, uint64_t to)
{
    FileMap*    map = tagfs_get_state()->files;
    uint64_t    ids[BINDER_SLICE_BATCH], jot = 0;
    FileRecord* was = NULL;
    uint32_t    n   = FileMapFiles(map, *from, ids, BINDER_SLICE_BATCH), i = 0;
    bool        d   = DocketWasFrom(*from, &jot, &was);
    for (uint32_t taken = 0; taken < BINDER_SLICE_BATCH;) {
        uint64_t m = i < n ? ids[i] : UINT64_MAX, j = d ? jot : UINT64_MAX;
        uint64_t f = m < j ? m : j;
        if (f == UINT64_MAX || f >= to) {
            FileRecordRelease(was);
            return false;
        }
        FileRecord* rec = NULL;
        if (j == f) {
            rec = was;
            was = NULL;
            d   = DocketWasFrom(f + 1u, &jot, &was);
        } else {
            rec = FileMapRecordHold(map, f, NULL);
        }
        i     += m == f;
        *from  = f + 1u;
        if (rec) {
            c->f[c->n++] = (BinderFile){ f, rec };
            w->nfiles++;
            taken++;
        }
        if (i == n && n == BINDER_SLICE_BATCH)
            break;
    }
    FileRecordRelease(was);
    return true;
}

/* The live files of the state after the last settled stitch whose ids lie in
 * `fids`, a batch at a time, the state lock let go between: no stitch settles
 * meanwhile — this is one step of the Settle's holder. OK or ERR_NO_MEMORY. */
error_t BinderSliceTake(BinderWork* w, const BinderFids* fids, uint32_t n)
{
    TagFSLock*   lock = &tagfs_get_state()->lock;
    BinderFiles* tail = NULL;
    for (uint32_t r = 0; r < n; r++) {
        uint64_t from = fids[r].from;
        for (bool more = true; more;) {
            if (!tail || tail->n + BINDER_SLICE_BATCH > BINDER_FILES_CHUNK) {
                BinderFiles* c = kmalloc(sizeof(BinderFiles));
                if (!c)
                    return ERR_NO_MEMORY;
                c->next = NULL;
                c->n    = 0;
                if (tail)
                    tail->next = c;
                else
                    w->files = c;
                tail = c;
            }
            TagFSLockTake(lock);
            more = SliceBatch(w, tail, &from, fids[r].to);
            TagFSLockDrop(lock);
        }
    }
    return OK;
}

void BinderSliceLetGo(BinderWork* w)
{
    while (w->files) {
        BinderFiles* next = w->files->next;
        for (uint32_t i = 0; i < w->files->n; i++)
            FileRecordRelease(w->files->f[i].rec);
        kfree(w->files);
        w->files = next;
    }
    w->nfiles = 0;
    kfree(w->ground);
    kfree(w->tground);
    kfree(w->dranges);
    w->ground   = NULL;
    w->nground  = 0;
    w->tground  = NULL;
    w->ntground = 0;
    w->dranges  = NULL;
    w->ndranges = 0;
    UseTextDrop(w->use);
    w->use = NULL;
}

/* Runs of ground sifted down a heap of `n` by first block. */
static void GroundSift(BinderGround* g, uint64_t at, uint64_t n)
{
    for (;;) {
        uint64_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < n && g[l].start > g[big].start)
            big = l;
        if (r < n && g[r].start > g[big].start)
            big = r;
        if (big == at)
            return;
        BinderGround t = g[at];
        g[at]  = g[big];
        g[big] = t;
        at     = big;
    }
}

uint64_t BinderGroundJoined(BinderGround* g, uint64_t n)
{
    for (uint64_t i = n / 2u; i-- > 0;)
        GroundSift(g, i, n);
    for (uint64_t end = n; end > 1u; end--) {
        BinderGround t = g[0];
        g[0]       = g[end - 1u];
        g[end - 1u] = t;
        GroundSift(g, 0, end - 1u);
    }
    uint64_t m = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (m && g[i].start <= g[m - 1u].start + g[m - 1u].count) {
            uint64_t end = g[i].start + g[i].count;
            if (end > g[m - 1u].start + g[m - 1u].count)
                g[m - 1u].count = end - g[m - 1u].start;
            continue;
        }
        g[m++] = g[i];
    }
    return m;
}

/* Every DATA run of the files of `c` onto the ground. */
static error_t ChunkGround(BinderWork* w, const BinderFiles* c, uint64_t* cap)
{
    for (uint32_t i = 0; i < c->n; i++) {
        StripWalk walk;
        Strip     s;
        StripWalkBegin(c->f[i].rec, &walk);
        while (StripWalkNextGround(&walk, &s)) {
            if (!BinderListRoom((void**)&w->ground, w->nground, cap, sizeof(BinderGround)))
                return ERR_NO_MEMORY;
            w->ground[w->nground++] = (BinderGround){ s.start, s.count };
        }
    }
    return OK;
}

/* The ground DIGESTS speaks for: every DATA run of every live file, and what
 * the mount holds for what rotted — in order of their first block, those that
 * meet joined; a Tally laid in makes its own once the leaves it lays are read
 * (binder_digests.c), and takes this one only where it checks — its slice is
 * every file then. OK or ERR_NO_MEMORY. */
error_t BinderGroundTake(BinderWork* w)
{
    uint64_t cap = 0;
    error_t  rc  = OK;
    if (w->tip && !BINDER_CHECKS)
        return OK;
    for (BinderFiles* c = w->files; rc == OK && c; c = c->next)
        rc = ChunkGround(w, c, &cap);
    if (rc != OK)
        return rc;
    for (uint64_t i = 0; i < g_binder.nheld; i++) {
        if (!BinderListRoom((void**)&w->ground, w->nground, &cap, sizeof(BinderGround)))
            return ERR_NO_MEMORY;
        w->ground[w->nground++] = g_binder.held[i];
    }
    w->nground = BinderGroundJoined(w->ground, w->nground);
    return OK;
}
