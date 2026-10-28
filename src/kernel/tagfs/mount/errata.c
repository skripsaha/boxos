#include "errata.h"
#include "../tagfs.h"
#include "ribbon.h"
#include "book_mount.h"
#include "volume_ribbon.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"

/* The most a varint takes, and an Undone's fields before its fids: seq_back,
 * why and the count. */
#define ERRATA_VARINT_MOST  10u
#define ERRATA_FIELDS       3u

static const TagSpelling s_undone_tag = { .key = "settle", .key_len = 6, .value = "undone", .value_len = 6 };

/* One Undone a stitch undone (§2.8): a stitch both reasons undo is undone once,
 * for the first found — the check of the data comes before the replay
 * (ribbon_transcribe.c, RibbonTranscribe) — every file of it named. */
error_t ErrataAdd(uint64_t seq, uint64_t why, uint64_t fid)
{
    BookErratum* e = NULL;
    for (uint32_t i = g_book.nerrata; i-- > 0 && !e;)
        if (g_book.errata[i].seq == seq)
            e = &g_book.errata[i];
    if (!e) {
        uint64_t cap = g_book.caperrata;
        if (!RibbonGrow((void**)&g_book.errata, g_book.nerrata, &cap, sizeof(BookErratum)))
            return ERR_NO_MEMORY;
        g_book.caperrata = (uint32_t)cap;
        e  = &g_book.errata[g_book.nerrata++];
        *e = (BookErratum){ .seq = seq, .why = why };
    }
    return RibbonListAdd(&e->fids, &e->nfids, &e->capfids, fid) ? OK : ERR_NO_MEMORY;
}

void ErrataLeaves(uint64_t fid)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_book.nerrata; i++) {
        BookErratum* e = &g_book.errata[i];
        uint32_t     n = 0;
        for (uint32_t k = 0; k < e->nfids; k++)
            if (e->fids[k] != fid)
                e->fids[n++] = e->fids[k];
        e->nfids = n;
        if (n)
            g_book.errata[kept++] = *e;
        else
            kfree(e->fids);
    }
    g_book.nerrata = kept;
}

static int ErrataOrder(const void* a, const void* b, void* ctx)
{
    const BookErratum* x = a;
    const BookErratum* y = b;
    (void)ctx;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

error_t ErrataClosed(void)
{
    uint32_t cap = 0;
    KSort(g_book.errata, g_book.nerrata, sizeof(BookErratum), ErrataOrder, NULL);
    for (uint32_t i = 0; i < g_book.nerrata; i++) {
        BookErratum* e = &g_book.errata[i];
        RibbonListSorted(e->fids, &e->nfids);
        for (uint32_t k = 0; k < e->nfids; k++)
            if (!RibbonListAdd(&g_book.wear, &g_book.nwear, &cap, e->fids[k]))
                return ERR_NO_MEMORY;
    }
    RibbonListSorted(g_book.wear, &g_book.nwear);
    g_book.worn = 0;
    if (g_book.nerrata)
        kprintf("[TagFS] the mount's verdict rolls back %u file(s) of %u stitch(es): held for the first stitch of the "
                "session that settles — an Undone of each, and settle:undone on each file that lives (§2.8)\n",
                g_book.nwear, g_book.nerrata);
    return OK;
}

/* Read by the mount's core at the door's opening while the Settle's holder
 * may already let the verdict go on another (ErrataSettled). */
bool ErrataHeld(void)
{
    return __atomic_load_n(&g_book.nerrata, __ATOMIC_ACQUIRE) != 0;
}

void ErrataAsk(void)
{
    if (ErrataHeld())
        SettleDemand(SETTLE_DEMAND_ERRATA);
}

/*
 * Whether the tag's refusal is the moment's, not the file's: the heap, the
 * ribbon's room, or a volume that takes no change now — a stopping machine's
 * (the halt's own cycle Inks its verdict all the same, ink.c InkVolumeBegins),
 * one gone, one only read, a door shut behind it. The cycle is refused for
 * it and the tag put on again by the next: the Undone is never laid without
 * it. What the file itself refuses for good — a warrant it asks, a record
 * with no room for one more tag — no cycle ever changes.
 */
static bool ErrataWornLater(error_t rc)
{
    return rc == ERR_NO_MEMORY || rc == ERR_WOULD_BLOCK || rc == ERR_TAGFS_NO_SPACE ||
           rc == ERR_DEVICE_NOT_READY || rc == ERR_TAGFS_READ_ONLY || rc == ERR_TAGFS_NOT_INITIALIZED;
}

error_t ErrataWorn(void)
{
    while (g_book.worn < g_book.nwear) {
        uint64_t fid = g_book.wear[g_book.worn];
        error_t  rc  = tagfs_add_tag_string(fid, &s_undone_tag, INK_UNWARRANTED);
        /* The ribbon's room comes back only by a cycle that settles, and on a
         * book no park can name it: a cycle refused for it is tried again a
         * window on (cadence.c, CadenceRunEnded). */
        if (rc == ERR_WOULD_BLOCK)
            rc = ERR_TAGFS_NO_SPACE;
        if (ErrataWornLater(rc))
            return rc;
        if (rc != OK && rc != ERR_FILE_NOT_FOUND)
            kprintf("[TagFS] file %llu, rolled back by the mount's verdict, can never take settle:undone (error %u): "
                    "its Undone is laid without it, and the mount's line names the file\n",
                    (unsigned long long)fid, (unsigned)rc);
        g_book.worn++;
    }
    return OK;
}

uint32_t ErrataUndones(void)
{
    return g_book.nerrata;
}

static uint64_t ErrataBodyOf(const BookErratum* e)
{
    return ((uint64_t)ERRATA_FIELDS + e->nfids) * ERRATA_VARINT_MOST;
}

uint64_t ErrataBytes(void)
{
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < g_book.nerrata; i++)
        bytes += STITCH_RECORD_HEAD_MOST + ErrataBodyOf(&g_book.errata[i]);
    return bytes;
}

uint64_t ErrataBodyMost(void)
{
    uint64_t most = 0;
    for (uint32_t i = 0; i < g_book.nerrata; i++)
        most = ErrataBodyOf(&g_book.errata[i]) > most ? ErrataBodyOf(&g_book.errata[i]) : most;
    return most;
}

uint32_t ErrataLay(uint8_t* out, uint32_t at, uint64_t seq, uint8_t* body, uint32_t* starts, uint32_t* n)
{
    for (uint32_t i = 0; i < g_book.nerrata; i++) {
        const BookErratum* e   = &g_book.errata[i];
        uint32_t           len = RibbonUndoneBodyPut(body, seq - e->seq, e->why, e->fids, e->nfids);
        starts[(*n)++] = at;
        at += RibbonRecordPut(out + at, RIBBON_UNDONE, 0, body, len);
    }
    return at;
}

void ErrataSettled(uint64_t seq)
{
    if (!ErrataHeld())
        return;
    kprintf("[TagFS] stitch %llu settled the mount's verdict: %u Undone(s), %u file(s) rolled back — the book's "
            "now, and memory lets it go\n", (unsigned long long)seq, g_book.nerrata, g_book.nwear);
    ErrataLetGo();
}

void ErrataLetGo(void)
{
    for (uint32_t i = 0; i < g_book.nerrata; i++)
        kfree(g_book.errata[i].fids);
    kfree(g_book.errata);
    kfree(g_book.wear);
    g_book.errata    = NULL;
    __atomic_store_n(&g_book.nerrata, 0u, __ATOMIC_RELEASE);
    g_book.caperrata = 0;
    g_book.wear      = NULL;
    g_book.nwear     = 0;
    g_book.worn      = 0;
}
