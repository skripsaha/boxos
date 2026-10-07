#include "helm.h"
#include "../ink/ink.h"
#include "baton.h"      /* the Knock that wakes a service point for a ready waiter */
#include "scheduler.h"   /* SchedulerHoldHere — a ready waiter's work is never switched away from */
#include "system_halt.h" /* HaltStopIfOwedHere — and a halt stops the core only after it */

// ----------------------------------------------------------------------------
// The open-file table
// ----------------------------------------------------------------------------

/*
 * ‼ THIS TABLE BELONGS TO THE MACHINE, NOT TO THE VOLUME.
 *
 * It used to be emptied by every mount — the lock re-initialised and the
 * buckets memset — which is three separate wrongs on a re-mount:
 *
 *   a lock another core is holding is re-initialised to "free", so the mutual
 *   exclusion it was providing is simply lost, silently;
 *
 *   every OpenFileEntry in it is dropped on the floor: they are kmalloc'd,
 *   the handles that own them still point at them, and nothing frees them;
 *
 *   and — the one that corrupts a file rather than losing memory — the next
 *   ofe_acquire for a file that already had an entry makes a SECOND one, with a
 *   second Helm. Two Helms for one file are two holders of its extents at
 *   once, which is what the Helm exists to make impossible.
 *
 * A mount brings up what is on the medium. Who has a file open is a fact about
 * this machine, and it is left alone.
 *
 * ‼ AND IT GROWS WITH WHAT IS OPEN. When it holds more entries than buckets it
 * doubles: the new table is allocated outside the lock and the entries moved
 * into it under the lock, and nothing is allocated under the lock at all — an
 * entry for a file not yet open is made before the lock is taken again to put
 * it in.
 *
 * The lock also keeps each entry's views, which the Settle cycle asks about
 * under the volume's state lock (HelmViewFloor): a leaf under state.
 */
#define OPEN_FILES_FIRST 32u

static OpenFileEntry  *g_open_first[OPEN_FILES_FIRST];
static OpenFileEntry **g_open_files   = g_open_first;
static uint32_t        g_open_buckets = OPEN_FILES_FIRST;
static uint32_t        g_open_count   = 0;
static uint32_t        g_open_regrown = 0;   /* bumped each time the entries move to a new table */
static TagFSLock g_open_table_lock = TAGFS_LOCK_INIT("openfiles", LOCK_RANK_LEAF);

/* The file's entry, under g_open_table_lock; NULL when it is not open. */
static OpenFileEntry *ofe_find(uint32_t file_id)
{
    for (OpenFileEntry *e = g_open_files[file_id % g_open_buckets]; e; e = e->next)
        if (e->file_id == file_id)
            return e;
    return NULL;
}

/* Twice the buckets, if the table still holds more entries than it has when
 * the lock is taken again. A table that cannot grow keeps its chains longer,
 * and nothing else is lost. */
static void ofe_grow(void)
{
    TagFSLockTake(&g_open_table_lock);
    uint32_t want = g_open_count > g_open_buckets ? g_open_buckets * 2u : 0;
    TagFSLockDrop(&g_open_table_lock);
    if (want == 0)
        return;

    OpenFileEntry **table = kmalloc(sizeof(OpenFileEntry *) * want);
    if (!table)
        return;
    memset(table, 0, sizeof(OpenFileEntry *) * want);

    OpenFileEntry **old = NULL;
    TagFSLockTake(&g_open_table_lock);
    if (g_open_buckets * 2u == want) {
        for (uint32_t b = 0; b < g_open_buckets; b++) {
            OpenFileEntry *e = g_open_files[b];
            while (e) {
                OpenFileEntry *next = e->next;
                e->next = table[e->file_id % want];
                table[e->file_id % want] = e;
                e = next;
            }
        }
        old            = g_open_files;
        g_open_files   = table;
        g_open_buckets = want;
        table          = NULL;
        g_open_regrown++;
    }
    TagFSLockDrop(&g_open_table_lock);
    kfree(table);
    if (old && old != g_open_first)
        kfree(old);
}

OpenFileEntry *ofe_acquire(uint32_t file_id)
{
    TagFSLockTake(&g_open_table_lock);
    OpenFileEntry *e = ofe_find(file_id);
    if (e) {
        e->ref_count++;
        TagFSLockDrop(&g_open_table_lock);
        return e;
    }
    TagFSLockDrop(&g_open_table_lock);

    OpenFileEntry *made = kmalloc(sizeof(OpenFileEntry));
    if (!made)
        return NULL;
    memset(made, 0, sizeof(*made));
    made->file_id   = file_id;
    made->ref_count = 1;
    TagFSLockInit(&made->helm.lock, "helm", LOCK_RANK_LEAF);

    /* Another core may have opened the file meanwhile: its entry is the one,
     * and this one is given back. */
    TagFSLockTake(&g_open_table_lock);
    e = ofe_find(file_id);
    if (e) {
        e->ref_count++;
    } else {
        uint32_t bucket = file_id % g_open_buckets;
        made->next = g_open_files[bucket];
        g_open_files[bucket] = made;
        g_open_count++;
        e    = made;
        made = NULL;
    }
    bool grow = g_open_count > g_open_buckets;
    TagFSLockDrop(&g_open_table_lock);
    kfree(made);
    if (grow)
        ofe_grow();
    return e;
}

/* The last reference frees the entry at once: every holder and waiter of its
 * Helm owns a handle, and so a reference, until it has let go. */
void ofe_release(OpenFileEntry *ofe)
{
    if (!ofe)
        return;

    TagFSLockTake(&g_open_table_lock);
    if (ofe->ref_count == 0) {
        uint32_t fid = ofe->file_id;
        TagFSLockDrop(&g_open_table_lock);
        panic("[TagFS] ofe_release: ref_count already zero for file_id=%u — double-release or corruption", fid);
    }
    bool last = --ofe->ref_count == 0;
    if (last) {
        for (OpenFileEntry **at = &g_open_files[ofe->file_id % g_open_buckets]; *at; at = &(*at)->next) {
            if (*at == ofe) {
                *at = ofe->next;
                g_open_count--;
                break;
            }
        }
    }
    TagFSLockDrop(&g_open_table_lock);
    if (last)
        kfree(ofe);
}

#if CONFIG_TAGFS_HOLD_GROUND
bool ofe_listed(const OpenFileEntry *ofe, uint32_t *out_refs)
{
    if (!ofe)
        return false;
    TagFSLockTake(&g_open_table_lock);
    OpenFileEntry *e     = ofe_find(ofe->file_id);
    bool           found = e == ofe;
    if (found && out_refs)
        *out_refs = e->ref_count;
    TagFSLockDrop(&g_open_table_lock);
    return found;
}
#endif

// ----------------------------------------------------------------------------
// The Helm
// ----------------------------------------------------------------------------

/* Holders and waiters, counted as they come and let go. */
static volatile uint64_t s_helm_owed;

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
static void HelmAnswerGone(HelmWaiter *w, uint64_t lost)
{
    while (w) {
        HelmWaiter *next = w->next;
        w->next = NULL;
        __atomic_sub_fetch(&s_helm_owed, 1u, __ATOMIC_ACQ_REL);
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
bool HelmAsk(OpenFileEntry *ofe, HelmWaiter *w)
{
    FileHelm *h = &ofe->helm;
    w->next = NULL;
    __atomic_add_fetch(&s_helm_owed, 1u, __ATOMIC_ACQ_REL);

    TagFSLockTake(&h->lock);
    bool gone = TagFSMediumLeft();
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
        HelmAnswerGone(w, 0);
    return mine;
}

bool HelmTry(OpenFileEntry *ofe, HelmWaiter *w)
{
    FileHelm *h = &ofe->helm;
    w->next = NULL;

    TagFSLockTake(&h->lock);
    bool mine = !TagFSMediumLeft() && h->holder == NULL && !h->parked;
    if (mine)
        h->holder = w;
    TagFSLockDrop(&h->lock);
    if (mine)
        __atomic_add_fetch(&s_helm_owed, 1u, __ATOMIC_ACQ_REL);
    return mine;
}

void HelmLetGo(OpenFileEntry *ofe, HelmWaiter *w)
{
    FileHelm *h = &ofe->helm;

    TagFSLockTake(&h->lock);
    if (h->holder != w) {
        TagFSLockDrop(&h->lock);
        kprintf("[TagFS] file %u: its Helm was let go of by a caller that does not hold it — "
                "the holder keeps it\n", ofe->file_id);
        return;
    }
    HelmWaiter *next = h->first;
    if (next && TagFSMediumLeft()) {
        h->parked = true;       /* the FIFO waits with the open book */
        next      = NULL;
    } else if (next) {
        h->first = next->next;
        if (!h->first)
            h->last = NULL;
        next->next = NULL;
    }
    h->holder = next;
    TagFSLockDrop(&h->lock);

    __atomic_sub_fetch(&s_helm_owed, 1u, __ATOMIC_ACQ_REL);
    if (next)
        HelmHand(next);
}

error_t HelmTakeNow(TagFSFileHandle *handle, HelmWaiter *here)
{
    memset(here, 0, sizeof(*here));
    if (HelmTry(handle->ofe, here))
        return OK;
    if (TagFSMediumLeft())
        return ERR_DEVICE_NOT_READY;    /* the departure said itself when it was latched */
    kprintf("[TagFS] file %u is being changed by the holder of its Helm — a caller in the "
            "kernel cannot be parked behind it, and is refused\n", handle->file_id);
    return ERR_BUSY;
}

bool HelmVacated(const OpenFileEntry *ofe)
{
    return __atomic_load_n(&ofe->helm.vacated, __ATOMIC_ACQUIRE);
}

void HelmVacate(uint32_t file_id)
{
    TagFSLockTake(&g_open_table_lock);
    OpenFileEntry *e = ofe_find(file_id);
    if (e)
        __atomic_store_n(&e->helm.vacated, true, __ATOMIC_RELEASE);
    TagFSLockDrop(&g_open_table_lock);
}

uint64_t HelmOwed(void)
{
    return __atomic_load_n(&s_helm_owed, __ATOMIC_ACQUIRE);
}

uint32_t HelmClaimedRuns(uint32_t file_id)
{
    TagFSLockTake(&g_open_table_lock);
    OpenFileEntry *e       = ofe_find(file_id);
    uint32_t       claimed = e ? e->helm.claimed_runs : 0;
    TagFSLockDrop(&g_open_table_lock);
    return claimed;
}

// ----------------------------------------------------------------------------
// Waiters whose work is run at a service point
// ----------------------------------------------------------------------------

/* The knock wakes the drain core so its loop reaches TagFSServiceIfPending,
 * where the listed work is run; the continuation itself stands over nothing. */
static void HelmKnocked(void *ctx);

static Knock       s_ready_knock = { .baton = { .next = NULL, .run = HelmKnocked, .ctx = NULL,
                                                .one_way = 0 },
                                     .raised = 0 };
static TagFSLock   s_ready_lock  = TAGFS_LOCK_INIT("helmready", LOCK_RANK_LEAF);
static HelmWaiter *s_ready_first;
static HelmWaiter *s_ready_last;
static volatile uint32_t s_ready_count;

static void HelmKnocked(void *ctx)
{
    (void)ctx;
    KnockOpen(&s_ready_knock);
}

void HelmReady(HelmWaiter *w)
{
    w->next = NULL;
    TagFSLockTake(&s_ready_lock);
    if (s_ready_last)
        s_ready_last->next = w;
    else
        s_ready_first = w;
    s_ready_last = w;
    __atomic_add_fetch(&s_ready_count, 1u, __ATOMIC_ACQ_REL);
    TagFSLockDrop(&s_ready_lock);
    KnockOn(&s_ready_knock);
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

static void HelmWalkBook(HelmBookOwed owed, uint64_t lost);

bool HelmReadyListed(void)
{
    return __atomic_load_n(&s_ready_count, __ATOMIC_ACQUIRE) != 0;
}

void HelmBookGivenUp(uint64_t lost)
{
    HelmWalkBook(HELM_BOOK_ANSWER, lost);
}

void HelmBookTakenUp(void)
{
    HelmWalkBook(HELM_BOOK_HAND, 0);
}

/*
 * Each waiter's work is a held span: a core an orderly halt asks to stop in
 * the middle of it owes its stop (idt.c), runs the work to its end, and stops
 * where the span is let go. Nothing after it would pay that debt — the core
 * goes back to its loop or its idle, which hold no span — and the halt's roll
 * call would wait for it for good.
 */
void HelmRunReady(void)
{
    for (uint32_t listed = __atomic_load_n(&s_ready_count, __ATOMIC_ACQUIRE); listed > 0;
         listed--) {
        TagFSLockTake(&s_ready_lock);
        HelmWaiter *w = s_ready_first;
        if (w) {
            s_ready_first = w->next;
            if (!s_ready_first)
                s_ready_last = NULL;
            w->next = NULL;
            __atomic_sub_fetch(&s_ready_count, 1u, __ATOMIC_ACQ_REL);
        }
        TagFSLockDrop(&s_ready_lock);
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
static OpenFileEntry *HelmWalkNext(OpenFileEntry *e, uint32_t *bucket)
{
    OpenFileEntry *n = e ? e->next : g_open_files[*bucket];
    while (!n && ++*bucket < g_open_buckets)
        n = g_open_files[*bucket];
    if (n)
        n->ref_count++;
    return n;
}

/* One Helm, as the book's fate says: its FIFO taken whole under its lock and
 * each waiter answered, or — parked — its first waiter handed the Helm. */
static void HelmBookFifo(OpenFileEntry *e, HelmBookOwed owed, uint64_t lost)
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
    HelmAnswerGone(chain, lost);
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
static void HelmWalkBook(HelmBookOwed owed, uint64_t lost)
{
    OpenFileEntry *at     = NULL;
    uint32_t       bucket = 0;
    uint32_t       shape  = 0;

    for (;;) {
        TagFSLockTake(&g_open_table_lock);
        if (!at || shape != g_open_regrown) {
            shape  = g_open_regrown;
            bucket = 0;
            if (at) {
                TagFSLockDrop(&g_open_table_lock);
                ofe_release(at);
                at = NULL;
                continue;
            }
        }
        OpenFileEntry *next = HelmWalkNext(at, &bucket);
        TagFSLockDrop(&g_open_table_lock);
        ofe_release(at);
        if (!next)
            return;
        HelmBookFifo(next, owed, lost);
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
void HelmViewTake(TagFSFileHandle *handle)
{
    OpenFileEntry *ofe = handle->ofe;

    TagFSLockTake(&g_open_table_lock);
    handle->view_gen = InkedGen();
    if (!handle->view_prev && ofe->views != handle) {
        handle->view_next = ofe->views;
        if (ofe->views)
            ofe->views->view_prev = handle;
        ofe->views = handle;
    }
    TagFSLockDrop(&g_open_table_lock);
}

void HelmViewLetGo(TagFSFileHandle *handle)
{
    OpenFileEntry *ofe = handle->ofe;

    TagFSLockTake(&g_open_table_lock);
    if (handle->view_prev)
        handle->view_prev->view_next = handle->view_next;
    else if (ofe->views == handle)
        ofe->views = handle->view_next;
    if (handle->view_next)
        handle->view_next->view_prev = handle->view_prev;
    handle->view_prev = NULL;
    handle->view_next = NULL;
    TagFSLockDrop(&g_open_table_lock);
}

/* A handle of an earlier mounting views a volume that is gone, and gens that
 * began again at the mount: it holds nothing of this one. */
uint64_t HelmViewFloor(uint32_t file_id)
{
    uint64_t floor = UINT64_MAX;
    uint32_t epoch = tagfs_mount_epoch();

    TagFSLockTake(&g_open_table_lock);
    OpenFileEntry *e = ofe_find(file_id);
    for (TagFSFileHandle *v = e ? e->views : NULL; v; v = v->view_next)
        if (v->mount_epoch == epoch && v->view_gen < floor)
            floor = v->view_gen;
    TagFSLockDrop(&g_open_table_lock);
    return floor;
}
