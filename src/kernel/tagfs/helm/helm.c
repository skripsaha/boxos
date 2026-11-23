#include "helm.h"
#include "engross.h"
#include "../ink/ink.h"
#include "baton.h"      /* the Knock that wakes a service point for a ready waiter */
#include "scheduler.h"   /* SchedulerHoldHere — a ready waiter's work is never switched away from */
#include "system_halt.h" /* HaltStopIfOwedHere — and a halt stops the core only after it */
#include "tagfs_volume.h"

// ----------------------------------------------------------------------------
// The open-file table
// ----------------------------------------------------------------------------


/* The file's entry, under helm.open_table_lock; NULL when it is not open. */
static OpenFileEntry *ofe_find(TagFSVolume* vol, uint64_t file_id)
{
    for (OpenFileEntry *e = vol->helm.open_files[file_id % vol->helm.open_buckets]; e; e = e->next)
        if (e->file_id == file_id)
            return e;
    return NULL;
}

/* Twice the buckets, if the table still holds more entries than it has when
 * the lock is taken again. A table that cannot grow keeps its chains longer,
 * and nothing else is lost. */
static void ofe_grow(TagFSVolume* vol)
{
    TagFSLockTake(&vol->helm.open_table_lock);
    uint32_t want = vol->helm.open_count > vol->helm.open_buckets ? vol->helm.open_buckets * 2u : 0;
    TagFSLockDrop(&vol->helm.open_table_lock);
    if (want == 0)
        return;

    OpenFileEntry **table = kmalloc(sizeof(OpenFileEntry *) * want);
    if (!table)
        return;
    memset(table, 0, sizeof(OpenFileEntry *) * want);

    OpenFileEntry **old = NULL;
    TagFSLockTake(&vol->helm.open_table_lock);
    if (vol->helm.open_buckets * 2u == want) {
        for (uint32_t b = 0; b < vol->helm.open_buckets; b++) {
            OpenFileEntry *e = vol->helm.open_files[b];
            while (e) {
                OpenFileEntry *next = e->next;
                e->next = table[e->file_id % want];
                table[e->file_id % want] = e;
                e = next;
            }
        }
        old            = vol->helm.open_files;
        vol->helm.open_files   = table;
        vol->helm.open_buckets = want;
        table          = NULL;
        vol->helm.open_regrown++;
    }
    TagFSLockDrop(&vol->helm.open_table_lock);
    kfree(table);
    if (old && old != vol->helm.open_first)
        kfree(old);
}

OpenFileEntry *ofe_acquire(TagFSVolume* vol, uint64_t file_id)
{
    TagFSLockTake(&vol->helm.open_table_lock);
    OpenFileEntry *e = ofe_find(vol, file_id);
    if (e) {
        e->ref_count++;
        TagFSLockDrop(&vol->helm.open_table_lock);
        return e;
    }
    TagFSLockDrop(&vol->helm.open_table_lock);

    OpenFileEntry *made = kmalloc(sizeof(OpenFileEntry));
    if (!made)
        return NULL;
    memset(made, 0, sizeof(*made));
    made->file_id   = file_id;
    made->ref_count = 1;
    TagFSLockInit(&made->helm.lock, "helm", LOCK_RANK_LEAF);

    /* Another core may have opened the file meanwhile: its entry is the one,
     * and this one is given back. */
    TagFSLockTake(&vol->helm.open_table_lock);
    e = ofe_find(vol, file_id);
    if (e) {
        e->ref_count++;
    } else {
        uint32_t bucket = (uint32_t)(file_id % vol->helm.open_buckets);
        made->next = vol->helm.open_files[bucket];
        vol->helm.open_files[bucket] = made;
        vol->helm.open_count++;
        e    = made;
        made = NULL;
    }
    bool grow = vol->helm.open_count > vol->helm.open_buckets;
    TagFSLockDrop(&vol->helm.open_table_lock);
    kfree(made);
    if (grow)
        ofe_grow(vol);
    return e;
}

/* The last reference frees the entry at once: every holder and waiter of its
 * Helm owns a handle, and so a reference, until it has let go. */
void ofe_release(TagFSVolume* vol, OpenFileEntry *ofe)
{
    if (!ofe)
        return;

    TagFSLockTake(&vol->helm.open_table_lock);
    if (ofe->ref_count == 0) {
        uint64_t fid = ofe->file_id;
        TagFSLockDrop(&vol->helm.open_table_lock);
        panic("[TagFS] ofe_release: ref_count already zero for file_id=%u — double-release or corruption", fid);
    }
    bool last = --ofe->ref_count == 0;
    if (last) {
        for (OpenFileEntry **at = &vol->helm.open_files[ofe->file_id % vol->helm.open_buckets]; *at; at = &(*at)->next) {
            if (*at == ofe) {
                *at = ofe->next;
                vol->helm.open_count--;
                break;
            }
        }
    }
    TagFSLockDrop(&vol->helm.open_table_lock);
    if (last)
        kfree(ofe);
}

#if CONFIG_TAGFS_HOLD_GROUND
bool ofe_listed(TagFSVolume* vol, const OpenFileEntry *ofe, uint32_t *out_refs)
{
    if (!ofe)
        return false;
    TagFSLockTake(&vol->helm.open_table_lock);
    OpenFileEntry *e     = ofe_find(vol, ofe->file_id);
    bool           found = e == ofe;
    if (found && out_refs)
        *out_refs = e->ref_count;
    TagFSLockDrop(&vol->helm.open_table_lock);
    return found;
}
#endif

// ----------------------------------------------------------------------------
// The Helm
// ----------------------------------------------------------------------------

/* The Helm is `next`'s: its hand is called on the core that let go, with no
 * TagFS lock held — an asynchronous job passes itself to the drain core, a
 * synchronous one is listed for a service point. */
static void HelmHand(HelmWaiter *next)
{
    next->hand(next);
}

/* Every waiter of the chain answered that the medium left, with `lost` — the
 * changes an open book given up took with it — and no TagFS lock held; each
 * one's answer may free it, so the next is read first. */
static void HelmAnswerGone(TagFSVolume* vol, HelmWaiter *w, uint64_t lost)
{
    while (w) {
        HelmWaiter *next = w->next;
        w->next = NULL;
        __atomic_sub_fetch(&vol->helm.helm_owed, 1u, __ATOMIC_ACQ_REL);
        w->gone(w, lost);
        w = next;
    }
}

/* The medium's departure is read under the Helm's lock: a waiter queued
 * before it was latched is held with the open book — handed on when the book
 * is taken up, answered by the walk that gives it up, which takes every
 * Helm's lock once the book's fate is set — and one that asks after it is
 * neither queued nor handed a free Helm: it is answered at once that the
 * medium is away, nothing of its own lost, before any of its work is begun —
 * a Helm handed out then took the work through the allocator and a cycle the
 * medium refused, and answered it "no room" or "write failed" (RT-13). A
 * parked Helm is held: its FIFO goes first. */
bool HelmAsk(TagFSVolume* vol, OpenFileEntry *ofe, HelmWaiter *w)
{
    FileHelm *h = &ofe->helm;
    w->next = NULL;
    __atomic_add_fetch(&vol->helm.helm_owed, 1u, __ATOMIC_ACQ_REL);

    TagFSLockTake(&h->lock);
    bool gone = TagFSMediumLeft(vol);
    bool mine = !gone && h->holder == NULL && !h->parked;
    if (mine) {
        h->holder = w;
    } else if (!gone) {
        if (h->last)
            h->last->next = w;
        else
            h->first = w;
        h->last = w;
    }
    TagFSLockDrop(&h->lock);
    if (gone)
        HelmAnswerGone(vol, w, 0);
    return mine;
}

/* The Helm for `w` when it is free; a held one noted when `note` — under the
 * Helm's lock, the one its holder's let-go reads the note under. */
static bool HelmTryAt(TagFSVolume* vol, OpenFileEntry *ofe, HelmWaiter *w, bool note)
{
    FileHelm *h = &ofe->helm;
    w->next = NULL;

    TagFSLockTake(&h->lock);
    bool mine = !TagFSMediumLeft(vol) && h->holder == NULL && !h->parked;
    if (mine)
        h->holder = w;
    else if (note)
        h->noted = true;
    TagFSLockDrop(&h->lock);
    if (mine)
        __atomic_add_fetch(&vol->helm.helm_owed, 1u, __ATOMIC_ACQ_REL);
    return mine;
}

bool HelmTry(TagFSVolume* vol, OpenFileEntry *ofe, HelmWaiter *w)
{
    return HelmTryAt(vol, ofe, w, false);
}

bool HelmTryNoted(TagFSVolume* vol, OpenFileEntry *ofe, HelmWaiter *w)
{
    return HelmTryAt(vol, ofe, w, true);
}

void HelmLetGo(TagFSVolume* vol, OpenFileEntry *ofe, HelmWaiter *w)
{
    FileHelm *h = &ofe->helm;

    TagFSLockTake(&h->lock);
    if (h->holder != w) {
        TagFSLockDrop(&h->lock);
        kprintf("[TagFS] file %llu: its Helm was let go of by a caller that does not hold it — "
                "the holder keeps it\n", ofe->file_id);
        return;
    }
    HelmWaiter *next = h->first;
    if (next && TagFSMediumLeft(vol)) {
        h->parked = true;       /* the FIFO waits with the open book */
        next      = NULL;
    } else if (next) {
        h->first = next->next;
        if (!h->first)
            h->last = NULL;
        next->next = NULL;
    }
    h->holder = next;
    bool noted = h->noted;
    h->noted   = false;
    TagFSLockDrop(&h->lock);

    __atomic_sub_fetch(&vol->helm.helm_owed, 1u, __ATOMIC_ACQ_REL);
    if (noted)
        EngrossAsked(vol);     /* a tail the service found under this holder waits for it */
    if (next)
        HelmHand(next);
}

error_t HelmTakeNow(TagFSVolume* vol, TagFSFileHandle *handle, HelmWaiter *here)
{
    memset(here, 0, sizeof(*here));
    if (HelmTry(vol, handle->ofe, here))
        return OK;
    if (TagFSMediumLeft(vol))
        return ERR_DEVICE_NOT_READY;    /* the departure said itself when it was latched */
    kprintf("[TagFS] file %llu is being changed by the holder of its Helm — a caller in the "
            "kernel cannot be parked behind it, and is refused\n", handle->file_id);
    return ERR_BUSY;
}

bool HelmVacated(const OpenFileEntry *ofe)
{
    return __atomic_load_n(&ofe->helm.vacated, __ATOMIC_ACQUIRE);
}

#ifdef CONFIG_VOLUME_TESTS
/*
 * ‼ THE STAND'S HOLDS, AND THE GIVE-UP THAT LISTS THEM, SHARE ONE LOCK.
 * A holder is laid here only while its file's `vacated` is clear, and the
 * give-up sets `vacated` and takes the file's holders out, both under the
 * table's lock: so either the give-up finds the holder here, or the holder
 * finds `vacated` set and is listed at once — never neither. Once the lock
 * is dropped neither side touches the other: a holder listed may run at once
 * on another core, end, and let its entry and its hold go with it.
 */
static HelmStandHold *s_stand_holds;   /* under helm.open_table_lock */

void HelmReadyOnceVacated(HelmStandHold *hold, OpenFileEntry *ofe, HelmWaiter *w)
{
    TagFSVolume* vol = TagFSHome();
    TagFSLockTake(&vol->helm.open_table_lock);
    bool now = __atomic_load_n(&ofe->helm.vacated, __ATOMIC_ACQUIRE);
    if (!now) {
        hold->ofe     = ofe;
        hold->holder  = w;
        hold->next    = s_stand_holds;
        s_stand_holds = hold;
    }
    TagFSLockDrop(&vol->helm.open_table_lock);
    if (now)
        HelmReady(vol, w);
}

/* The stand's holds of the file's entry, taken out of the list: under
 * helm.open_table_lock, and listed by the caller once it is dropped. */
static HelmStandHold *HelmStandHoldsOf(const OpenFileEntry *e)
{
    HelmStandHold *taken = NULL;
    for (HelmStandHold **at = &s_stand_holds; *at;) {
        HelmStandHold *h = *at;
        if (h->ofe != e) {
            at = &h->next;
            continue;
        }
        *at     = h->next;
        h->next = taken;
        taken   = h;
    }
    return taken;
}

/* Each holder taken listed — its next read first: a holder listed may run
 * and end, its hold with it, before this one goes on. */
static void HelmStandHoldsReady(TagFSVolume* vol, HelmStandHold *h)
{
    while (h) {
        HelmStandHold *next = h->next;
        HelmReady(vol, h->holder);
        h = next;
    }
}
#endif

void HelmVacate(TagFSVolume* vol, uint64_t file_id)
{
    TagFSLockTake(&vol->helm.open_table_lock);
    OpenFileEntry *e = ofe_find(vol, file_id);
    if (e)
        __atomic_store_n(&e->helm.vacated, true, __ATOMIC_RELEASE);
#ifdef CONFIG_VOLUME_TESTS
    HelmStandHold *held = e ? HelmStandHoldsOf(e) : NULL;
#endif
    TagFSLockDrop(&vol->helm.open_table_lock);
#ifdef CONFIG_VOLUME_TESTS
    HelmStandHoldsReady(vol, held);
#endif
}

uint64_t HelmOwed(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->helm.helm_owed, __ATOMIC_ACQUIRE);
}

// ----------------------------------------------------------------------------
// Waiters whose work is run at a service point
// ----------------------------------------------------------------------------

/* The knock wakes the drain core so its loop reaches TagFSServiceIfPending,
 * where the listed work is run; the continuation itself stands over nothing. */
static void HelmKnocked(void *ctx);


static void HelmKnocked(void *ctx)
{
    TagFSVolume* vol = ctx;
    KnockOpen(&vol->helm.ready_knock);
}

void HelmReady(TagFSVolume* vol, HelmWaiter *w)
{
    w->next = NULL;
    TagFSLockTake(&vol->helm.ready_lock);
    if (vol->helm.ready_last)
        vol->helm.ready_last->next = w;
    else
        vol->helm.ready_first = w;
    vol->helm.ready_last = w;
    __atomic_add_fetch(&vol->helm.ready_count, 1u, __ATOMIC_ACQ_REL);
    TagFSLockDrop(&vol->helm.ready_lock);
    KnockOn(&vol->helm.ready_knock);
}

/*
 * ‼ THE OPEN BOOK'S FATE IS DONE WHERE IT IS DECIDED, NEVER LEFT IN A SLOT.
 *
 * It used to be one word, set where the fate was decided and walked at the
 * next service point, the last one written winning. A fate written over
 * another lost the first: a book given up — its waiters owed the answer that
 * their changes were lost — then taken up before any service point walked it,
 * and the HAND that replaced the ANSWER handed the Helm to waiters who went on
 * to write into a volume the give-up was about to let go of (Z07-2). And a
 * fate walked late reached waiters the book never had: an ANSWER walked after
 * the volume was read again answered a writer of the NEW volume that its
 * changes were lost (Z11-12). So the walk is made in the call that decides,
 * on the core attending the medium — while a book given up is still latched,
 * so nobody is queued behind its Helms meanwhile; after a book taken up is let
 * go of, so the parked Helms are handed on to a volume that takes writes.
 */
typedef enum {
    HELM_BOOK_ANSWER = 1,   /* given up: every waiter answered */
    HELM_BOOK_HAND,         /* taken up: every parked Helm handed on */
} HelmBookOwed;

static void HelmWalkBook(TagFSVolume* vol, HelmBookOwed owed, uint64_t lost);

bool HelmReadyListed(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->helm.ready_count, __ATOMIC_ACQUIRE) != 0;
}

void HelmBookGivenUp(TagFSVolume* vol, uint64_t lost)
{
    HelmWalkBook(vol, HELM_BOOK_ANSWER, lost);
}

void HelmBookTakenUp(TagFSVolume* vol)
{
    HelmWalkBook(vol, HELM_BOOK_HAND, 0);
}

/*
 * Each waiter's work is a held span: a core an orderly halt asks to stop in
 * the middle of it owes its stop (idt.c), runs the work to its end, and stops
 * where the span is let go. Nothing after it would pay that debt — the core
 * goes back to its loop or its idle, which hold no span — and the halt's roll
 * call would wait for it for good.
 */
void HelmRunReady(TagFSVolume* vol)
{
    for (uint32_t listed = __atomic_load_n(&vol->helm.ready_count, __ATOMIC_ACQUIRE); listed > 0;
         listed--) {
        TagFSLockTake(&vol->helm.ready_lock);
        HelmWaiter *w = vol->helm.ready_first;
        if (w) {
            vol->helm.ready_first = w->next;
            if (!vol->helm.ready_first)
                vol->helm.ready_last = NULL;
            w->next = NULL;
            __atomic_sub_fetch(&vol->helm.ready_count, 1u, __ATOMIC_ACQ_REL);
        }
        TagFSLockDrop(&vol->helm.ready_lock);
        if (!w)
            return;

        SchedulerHoldHere();
        w->run(w);
        SchedulerLetGoHere();
        HaltStopIfOwedHere("the Helm work it ran at a service point");
    }
}

// ----------------------------------------------------------------------------
// The FIFOs of a medium that left: held with the open book (RT-13)
// ----------------------------------------------------------------------------

/* The entry the walk visits after `e` — the next of its bucket, or the first
 * of a later bucket — with a reference of the walk's own, so it stays listed
 * while its Helm's lock is taken; NULL at the end of the table. Under the
 * table's lock, the table not regrown since `e` was visited. */
static OpenFileEntry *HelmWalkNext(TagFSVolume* vol, OpenFileEntry *e, uint32_t *bucket)
{
    OpenFileEntry *n = e ? e->next : vol->helm.open_files[*bucket];
    while (!n && ++*bucket < vol->helm.open_buckets)
        n = vol->helm.open_files[*bucket];
    if (n)
        n->ref_count++;
    return n;
}

/* One Helm, as the book's fate says: its FIFO taken whole under its lock and
 * each waiter answered, or — parked — its first waiter handed the Helm. */
static void HelmBookFifo(TagFSVolume* vol, OpenFileEntry *e, HelmBookOwed owed, uint64_t lost)
{
    FileHelm   *h     = &e->helm;
    HelmWaiter *chain = NULL;
    HelmWaiter *next  = NULL;

    TagFSLockTake(&h->lock);
    if (owed == HELM_BOOK_ANSWER) {
        chain    = h->first;
        h->first = NULL;
        h->last  = NULL;
    } else if (h->parked && h->holder == NULL && h->first) {
        next     = h->first;
        h->first = next->next;
        if (!h->first)
            h->last = NULL;
        next->next = NULL;
        h->holder  = next;
    }
    h->parked = false;
    TagFSLockDrop(&h->lock);

    if (next)
        HelmHand(next);
    HelmAnswerGone(vol, chain, lost);
}

/*
 * Every open file's Helm, with nothing allocated: the walk holds a reference
 * on the entry it stands on, so the entry's place in its bucket stays, and
 * takes the next one under the table's lock. A table regrown meanwhile has
 * moved every entry to another bucket, and the walk begins again — an entry
 * visited twice finds its FIFO empty, or no longer parked, the second time.
 * It ends: no waiter is queued and no Helm parked but while the departure is
 * latched, and a table only regrows by doubling.
 */
static void HelmWalkBook(TagFSVolume* vol, HelmBookOwed owed, uint64_t lost)
{
    OpenFileEntry *at     = NULL;
    uint32_t       bucket = 0;
    uint32_t       shape  = 0;

    for (;;) {
        TagFSLockTake(&vol->helm.open_table_lock);
        if (!at || shape != vol->helm.open_regrown) {
            shape  = vol->helm.open_regrown;
            bucket = 0;
            if (at) {
                TagFSLockDrop(&vol->helm.open_table_lock);
                ofe_release(vol, at);
                at = NULL;
                continue;
            }
        }
        OpenFileEntry *next = HelmWalkNext(vol, at, &bucket);
        TagFSLockDrop(&vol->helm.open_table_lock);
        ofe_release(vol, at);
        if (!next)
            return;
        HelmBookFifo(vol, next, owed, lost);
        at = next;
    }
}

// ----------------------------------------------------------------------------
// Views
// ----------------------------------------------------------------------------

/* The gen is read under the table's lock and before the record is copied or
 * loaded, so every block the copy names was named by a record no older than
 * the view: an Ink that frees one of them comes after it, and rests while the
 * handle is listed. A handle already listed only moves its view on. */
void HelmViewTake(TagFSVolume* vol, TagFSFileHandle *handle)
{
    OpenFileEntry *ofe = handle->ofe;

    TagFSLockTake(&vol->helm.open_table_lock);
    handle->view_gen = InkedGen(vol);
    if (!handle->view_prev && ofe->views != handle) {
        handle->view_next = ofe->views;
        if (ofe->views)
            ofe->views->view_prev = handle;
        ofe->views = handle;
    }
    TagFSLockDrop(&vol->helm.open_table_lock);
}

void HelmViewLetGo(TagFSVolume* vol, TagFSFileHandle *handle)
{
    OpenFileEntry *ofe = handle->ofe;

    TagFSLockTake(&vol->helm.open_table_lock);
    if (handle->view_prev)
        handle->view_prev->view_next = handle->view_next;
    else if (ofe->views == handle)
        ofe->views = handle->view_next;
    if (handle->view_next)
        handle->view_next->view_prev = handle->view_prev;
    handle->view_prev = NULL;
    handle->view_next = NULL;
    TagFSLockDrop(&vol->helm.open_table_lock);
}

/* A handle of an earlier mounting views a volume that is gone, and gens that
 * began again at the mount: it holds nothing of this one. */
uint64_t HelmViewFloor(TagFSVolume* vol, uint64_t file_id)
{
    uint64_t floor = UINT64_MAX;
    uint32_t epoch = tagfs_mount_epoch(vol);

    TagFSLockTake(&vol->helm.open_table_lock);
    OpenFileEntry *e = ofe_find(vol, file_id);
    for (TagFSFileHandle *v = e ? e->views : NULL; v; v = v->view_next)
        if (v->mount_epoch == epoch && v->view_gen < floor)
            floor = v->view_gen;
    TagFSLockDrop(&vol->helm.open_table_lock);
    return floor;
}

/* The volume's helm part as it begins: what was not nothing. */
void HelmVolumeBegin(TagFSVolume* vol)
{
    vol->helm.open_files = vol->helm.open_first;
    vol->helm.open_buckets = OPEN_FILES_FIRST;
    vol->helm.open_table_lock = (TagFSLock)TAGFS_LOCK_INIT("openfiles", LOCK_RANK_LEAF);
    vol->helm.ready_knock = (Knock){ .baton = { .next = NULL, .run = HelmKnocked, .ctx = vol,
                                                .one_way = 0 },
                                     .raised = 0 };
    vol->helm.ready_lock = (TagFSLock)TAGFS_LOCK_INIT("helmready", LOCK_RANK_LEAF);
}
