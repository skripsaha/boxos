#include "errata.h"
#include "../tagfs.h"
#include "ribbon.h"
#include "book_mount.h"
#include "volume_ribbon.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../ink/ink.h"
#include "tagfs_volume.h"

/* The most a varint takes, and an Undone's fields before its fids: seq_back,
 * why and the count. */
#define ERRATA_VARINT_MOST  10u
#define ERRATA_FIELDS       3u

/* One Undone a stitch undone (§2.8): a stitch both reasons undo is undone once,
 * for the first found — the check of the data comes before the replay
 * (ribbon_transcribe.c, RibbonTranscribe) — every file of it named. */
error_t ErrataAdd(TagFSVolume* vol, uint64_t seq, uint64_t why, uint64_t fid)
{
    BookErratum* e = NULL;
    for (uint32_t i = vol->book.nerrata; i-- > 0 && !e;)
        if (vol->book.errata[i].seq == seq)
            e = &vol->book.errata[i];
    if (!e) {
        uint64_t cap = vol->book.caperrata;
        if (!RibbonGrow((void**)&vol->book.errata, vol->book.nerrata, &cap, sizeof(BookErratum)))
            return ERR_NO_MEMORY;
        vol->book.caperrata = (uint32_t)cap;
        e  = &vol->book.errata[vol->book.nerrata++];
        *e = (BookErratum){ .seq = seq, .why = why };
    }
    return RibbonListAdd(&e->fids, &e->nfids, &e->capfids, fid) ? OK : ERR_NO_MEMORY;
}

void ErrataLeaves(TagFSVolume* vol, uint64_t fid)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < vol->book.nerrata; i++) {
        BookErratum* e = &vol->book.errata[i];
        uint32_t     n = 0;
        for (uint32_t k = 0; k < e->nfids; k++)
            if (e->fids[k] != fid)
                e->fids[n++] = e->fids[k];
        e->nfids = n;
        if (n)
            vol->book.errata[kept++] = *e;
        else
            kfree(e->fids);
    }
    vol->book.nerrata = kept;
}

static int ErrataOrder(const void* a, const void* b, void* ctx)
{
    const BookErratum* x = a;
    const BookErratum* y = b;
    (void)ctx;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

error_t ErrataClosed(TagFSVolume* vol)
{
    uint32_t cap = 0;
    KSort(vol->book.errata, vol->book.nerrata, sizeof(BookErratum), ErrataOrder, NULL);
    for (uint32_t i = 0; i < vol->book.nerrata; i++) {
        BookErratum* e = &vol->book.errata[i];
        RibbonListSorted(e->fids, &e->nfids);
        for (uint32_t k = 0; k < e->nfids; k++)
            if (!RibbonListAdd(&vol->book.mark, &vol->book.nmark, &cap, e->fids[k]))
                return ERR_NO_MEMORY;
    }
    RibbonListSorted(vol->book.mark, &vol->book.nmark);
    vol->book.marked = 0;
    if (vol->book.nerrata)
        kprintf("[TagFS] the mount's verdict rolls back %u file(s) of %u stitch(es): held for the first stitch of the "
                "session that settles — an Undone of each, and each file that lives marked rolled back in its record (§2.8)\n",
                vol->book.nmark, vol->book.nerrata);
    return OK;
}

/* Read by the mount's core at the door's opening while the Settle's holder
 * may already let the verdict go on another (ErrataSettled). */
bool ErrataHeld(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->book.nerrata, __ATOMIC_ACQUIRE) != 0;
}

void ErrataAsk(TagFSVolume* vol)
{
    if (ErrataHeld(vol))
        SettleDemand(vol, SETTLE_DEMAND_ERRATA);
}

/*
 * Whether the mark's refusal is the moment's, not the file's: the heap, the
 * ribbon's room, or a volume that takes no change now — a stopping machine's
 * (the halt's own cycle Inks its verdict all the same, ink.c InkVolumeBegins),
 * one gone, one only read, a door shut behind it. The cycle is refused for
 * it and the mark made again by the next: the Undone is never laid without
 * it. What the file itself refuses for good no cycle ever changes.
 */
static bool ErrataMarkedLater(error_t rc)
{
    return rc == ERR_NO_MEMORY || rc == ERR_WOULD_BLOCK || rc == ERR_TAGFS_NO_SPACE ||
           rc == ERR_DEVICE_NOT_READY || rc == ERR_TAGFS_READ_ONLY || rc == ERR_TAGFS_NOT_INITIALIZED;
}

error_t ErrataMarked(TagFSVolume* vol)
{
    while (vol->book.marked < vol->book.nmark) {
        uint64_t   fid = vol->book.mark[vol->book.marked];
        InkReceipt receipt;
        error_t    rc  = InkUndone(vol, fid, &receipt);
        /* The ribbon's room comes back only by a cycle that settles, and on a
         * book no park can name it: a cycle refused for it is tried again a
         * window on (cadence.c, CadenceRunEnded). */
        if (rc == ERR_WOULD_BLOCK)
            rc = ERR_TAGFS_NO_SPACE;
        if (ErrataMarkedLater(rc))
            return rc;
        if (rc != OK && rc != ERR_FILE_NOT_FOUND)
            kprintf("[TagFS] file %llu, rolled back by the mount's verdict, can never be marked so in its record "
                    "(error %u): its Undone is laid without the mark, and the mount's line names the file\n",
                    (unsigned long long)fid, (unsigned)rc);
        vol->book.marked++;
    }
    return OK;
}

uint32_t ErrataUndones(TagFSVolume* vol)
{
    return vol->book.nerrata;
}

static uint64_t ErrataBodyOf(const BookErratum* e)
{
    return ((uint64_t)ERRATA_FIELDS + e->nfids) * ERRATA_VARINT_MOST;
}

uint64_t ErrataBytes(TagFSVolume* vol)
{
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < vol->book.nerrata; i++)
        bytes += STITCH_RECORD_HEAD_MOST + ErrataBodyOf(&vol->book.errata[i]);
    return bytes;
}

uint64_t ErrataBodyMost(TagFSVolume* vol)
{
    uint64_t most = 0;
    for (uint32_t i = 0; i < vol->book.nerrata; i++)
        most = ErrataBodyOf(&vol->book.errata[i]) > most ? ErrataBodyOf(&vol->book.errata[i]) : most;
    return most;
}

uint32_t ErrataLay(TagFSVolume* vol, uint8_t* out, uint32_t at, uint64_t seq, uint8_t* body, uint32_t* starts, uint32_t* n)
{
    for (uint32_t i = 0; i < vol->book.nerrata; i++) {
        const BookErratum* e   = &vol->book.errata[i];
        uint32_t           len = RibbonUndoneBodyPut(body, seq - e->seq, e->why, e->fids, e->nfids);
        starts[(*n)++] = at;
        at += RibbonRecordPut(out + at, RIBBON_UNDONE, 0, body, len);
    }
    return at;
}

void ErrataSettled(TagFSVolume* vol, uint64_t seq)
{
    if (!ErrataHeld(vol))
        return;
    kprintf("[TagFS] stitch %llu settled the mount's verdict: %u Undone(s), %u file(s) rolled back — the book's "
            "now, and memory lets it go\n", (unsigned long long)seq, vol->book.nerrata, vol->book.nmark);
    ErrataLetGo(vol);
}

void ErrataLetGo(TagFSVolume* vol)
{
    for (uint32_t i = 0; i < vol->book.nerrata; i++)
        kfree(vol->book.errata[i].fids);
    kfree(vol->book.errata);
    kfree(vol->book.mark);
    vol->book.errata    = NULL;
    __atomic_store_n(&vol->book.nerrata, 0u, __ATOMIC_RELEASE);
    vol->book.caperrata = 0;
    vol->book.mark      = NULL;
    vol->book.nmark     = 0;
    vol->book.marked      = 0;
}
