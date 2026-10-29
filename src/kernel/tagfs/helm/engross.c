#include "engross.h"
#include "../settle/escheat.h"
#include "helm.h"
#include "../file_map/file_map.h"
#include "../mount/book_mount.h"
#include "../settle/binder.h"
#include "../settle/loose_leaves.h"

/*
 * Engross (engross.h): the service of the Helm that lays the tails a Tally
 * waits for, a turn of a few files at a time, and the guard of the tags the
 * loader reads a file by.
 */

#define ENGROSS_TURN   8u                      /* tails a service point tries before it gives its core back */
#define ENGROSS_BATCH  32u                     /* file ids one look at the map takes */
#define ENGROSS_LOOK   (4u * ENGROSS_BATCH)    /* files a service point looks at before it gives its core back */
#define ENGROSS_KNOWN  16u                     /* refusals of a file's own the service keeps */

/* A walk of the files from `from` on, and what it left: the files whose tail
 * stays in Slips — their Helm another's, the ribbon's room on its way, or a
 * refusal — and the first of those refused. */
typedef struct {
    uint64_t from;        /* the next file id; UINT64_MAX once the map has no more */
    uint32_t left;
    uint32_t refused;     /* of `left`, refused by anything but a held Helm or the ribbon's room */
    uint32_t boot;        /* of `refused`, the file the loader reads the kernel by: no call changes it */
    uint64_t first_fid;
    error_t  first;
    uint64_t act;         /* the asks of Escheat this walk cuts tails for — 0 for none (EscheatTailsActing) */
} EngrossWalkState;

/* A refusal that is the file's own — its last block unreadable, the file the
 * loader reads the kernel by, a file the mount locked — made to the record
 * that Ink `gen` made, in mounting `epoch`: tried again only once the record
 * changes, so a rotten tail is neither read nor said at every turn — or by
 * the walk that answers an ask of Escheat. */
typedef struct {
    uint64_t fid;
    uint64_t gen;
    uint64_t epoch;
    error_t  rc;
} EngrossRefusal;

static void EngrossTurn(HelmWaiter* w);
static void EngrossListed(void);

static HelmWaiter        s_service = { .run = EngrossTurn };
static volatile uint32_t s_listed;
static volatile uint64_t s_asked;      /* asks made: each EngrossAsked counts one */
/* The service point's alone — one listing runs at a time (s_listed): */
static uint64_t          s_pass;       /* the asks the walk under way answers */
static EngrossWalkState  s_walk;
static EngrossRefusal    s_known[ENGROSS_KNOWN];
static uint32_t          s_known_next;
static EngrossWalkState  s_said;       /* the last walk's leavings said */
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
static volatile uint64_t s_inked;      /* the last Engross's Ink, 0 for none */
#endif

/* Whether the file's record keeps Slips over its last block now, *gen the Ink
 * that made the record. */
static bool EngrossOwedBy(uint64_t fid, uint64_t* gen)
{
    RecordWhere where = { 0 };
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, fid, &where);
    bool        owed  = rec && rec->slip;
    FileRecordRelease(rec);
    *gen = where.gen;
    return owed;
}

/* The file opened for the kernel's own change of it: one that lays a tail
 * where it already is the file's carries the warrant a file the system keeps
 * asks for. NULL and *rc for one the mount locked (ERR_TAGFS_CORRUPTED, as
 * its open answers every caller) or no memory. */
static TagFSFileHandle* EngrossOpen(uint64_t fid, error_t* rc)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_WRITE);
    *rc = h ? OK : BookFileLocked(fid) ? ERR_TAGFS_CORRUPTED : ERR_NO_MEMORY;
    if (h)
        h->warrant = INK_WARRANTED;
    return h;
}

/* One file's tail laid in its block, its Helm taken only when free — a Helm
 * found held asks the service again at its let-go (HelmTryNoted): OK — and OK
 * for a file with no tail left, or given up — ERR_BUSY for a Helm another
 * holds, or the refusal. */
static error_t EngrossFile(uint64_t fid)
{
    error_t          rc   = OK;
    TagFSFileHandle* h    = EngrossOpen(fid, &rc);
    HelmWaiter       here = { 0 };
    bool             laid = false;
    if (!h)
        return rc;
    rc = HelmTryNoted(h->ofe, &here) ? OK : ERR_BUSY;
    if (rc == OK) {
        rc = HelmEngross(h, &laid);
        HelmLetGo(h->ofe, &here);
    }
    tagfs_close(h);
    return rc == ERR_FILE_NOT_FOUND ? OK : rc;
}

/* Whether a refusal passes with time rather than with the file: a Helm held,
 * the ribbon's room on its way, the heap, the ground others give back. */
static bool EngrossPasses(error_t rc)
{
    return rc == ERR_BUSY || rc == ERR_WOULD_BLOCK || rc == ERR_NO_MEMORY || rc == ERR_TAGFS_NO_SPACE;
}

/* The file cut to `size` with the act's warrant — the act is asked by a cabin
 * wearing system or god alone (escheat.h), so a file the system keeps is cut
 * as the cabin could cut it itself; the file the loader reads the kernel by is
 * changed by no call (ink.h InkGuardSays). OK, or the refusal. */
static error_t EngrossCut(uint64_t fid, uint64_t size)
{
    error_t          rc = OK;
    TagFSFileHandle* h  = EngrossOpen(fid, &rc);
    HelmWaiter       here;
    if (!h)
        return rc;
    rc = HelmTakeNow(h, &here);
    if (rc == OK) {
        uint64_t was = 0;
        do {
            rc = HelmSpan(h, size, &was);
        } while (HelmRoomCameBack(&rc));
        HelmLetGo(h->ofe, &here);
    }
    tagfs_close(h);
    return rc;
}

/*
 * ‼ A TAIL THAT CAN NEVER BE LAID IS LOST ONLY BY ESCHEAT (spec_v2 §16.5 п.20).
 * A refusal that is the file's own — its last block unreadable — holds the
 * Tally back for that file alone, said, until the act is asked
 * (settle/escheat.h); then the tail is lost — the file cut to the end of its
 * last whole block, the Slip over the block it began going with the block,
 * said — and the Tally goes on. The act reaches as far as one walk of every
 * file begun after it was asked — the service's or the holder's, whichever
 * ends first (EscheatTailsActing, EscheatTailsWalked) — and no further: a
 * file that begins to refuse once that walk is done waits for an ask of its
 * own. A refusal that may pass — a medium that would not answer this once
 * (ERR_READ_FAILED), as much as a held Helm or no room — is never the act's.
 * OK when the tail was let go.
 */
static error_t EngrossEscheat(uint64_t fid, error_t why)
{
    RecordWhere where = { 0 };
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, fid, &where);
    uint64_t    size  = rec ? rec->size : 0;
    FileRecordRelease(rec);
    if (!rec || EngrossPasses(why) || why == ERR_READ_FAILED)
        return why;
    uint64_t whole = size / TAGFS_BLOCK_SIZE * TAGFS_BLOCK_SIZE;
    error_t  rc    = EngrossCut(fid, whole);
    if (rc == OK)
        kprintf("[TagFS] Escheat: fid %llu's tail of %llu byte(s) can never be laid in its block (error %u) — lost: "
                "the file ends at its last whole block, %llu byte(s)\n", (unsigned long long)fid,
                (unsigned long long)(size - whole), (unsigned)why, (unsigned long long)whole);
    else
        kprintf("[TagFS] Escheat: fid %llu's tail can never be laid in its block (error %u), and the file would not "
                "be cut to its last whole block (error %u) — the Tally waits for it still\n",
                (unsigned long long)fid, (unsigned)why, (unsigned)rc);
    if (rc == OK)
        EscheatTailLost();
    return rc == OK ? OK : why;
}

static const EngrossRefusal* EngrossKnownOf(uint64_t fid, uint64_t gen)
{
    uint64_t epoch = tagfs_mount_epoch();
    for (uint32_t i = 0; i < ENGROSS_KNOWN; i++)
        if (s_known[i].rc != OK && s_known[i].fid == fid && s_known[i].gen == gen && s_known[i].epoch == epoch)
            return &s_known[i];
    return NULL;
}

uint32_t EngrossRefusedNow(uint32_t* boot)
{
    uint32_t b = __atomic_load_n(&s_said.boot, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&s_said.refused, __ATOMIC_RELAXED);
    *boot      = b;
    return r > b ? r - b : 0;
}

static void EngrossKnow(uint64_t fid, uint64_t gen, error_t rc)
{
    s_known[s_known_next++ % ENGROSS_KNOWN] = (EngrossRefusal){ .fid = fid, .gen = gen,
                                                                .epoch = tagfs_mount_epoch(), .rc = rc };
}

/* One file of the walk: its tail tried when it has one — or, for the service
 * (`known`), passed over when the same record refused it before. OK, or
 * ERR_DEVICE_NOT_READY when the medium left. */
static error_t EngrossOne(EngrossWalkState* w, uint64_t fid, bool known, uint32_t* tails)
{
    uint64_t              gen = 0;
    const EngrossRefusal* was = NULL;
    if (!EngrossOwedBy(fid, &gen))
        return OK;
    (*tails)--;
    was        = known ? EngrossKnownOf(fid, gen) : NULL;
    error_t rc = was && !w->act ? was->rc : EngrossFile(fid);
    if (w->act && rc != OK && rc != ERR_DEVICE_NOT_READY)
        rc = EngrossEscheat(fid, rc);
    if (rc == OK)
        return OK;
    w->left++;
    if (rc == ERR_DEVICE_NOT_READY)
        return rc;
    if (rc == ERR_BUSY || rc == ERR_WOULD_BLOCK)
        return OK;
    w->boot += fid == g_book.boot_fid ? 1u : 0u;
    if (w->refused++ == 0) {
        w->first_fid = fid;
        w->first     = rc;
    }
    if (known && !was && !EngrossPasses(rc))
        EngrossKnow(fid, gen, rc);
    return OK;
}

/*
 * From w->from on, the files whose tail lies in Slips laid in their blocks —
 * `tails` of them tried and `looks` files looked at, at the most. A file's
 * refusal is its own: counted in what the walk leaves, and the walk goes on
 * past it. OK; ERR_DEVICE_NOT_READY, the walk stopped, when the medium left or
 * the volume takes no write a Tally could go on with.
 */
static error_t EngrossWalk(EngrossWalkState* w, uint32_t tails, uint32_t looks, bool known)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    ids[ENGROSS_BATCH];
    while (tails > 0 && looks > 0 && w->from != UINT64_MAX) {
        uint32_t n = FileMapFiles(fs->files, w->from, ids, looks < ENGROSS_BATCH ? looks : ENGROSS_BATCH);
        if (n == 0) {
            w->from = UINT64_MAX;
            break;
        }
        for (uint32_t i = 0; i < n && tails > 0; i++) {
            w->from = ids[i] + 1u;
            looks--;
            error_t rc = BinderNoWrite() ? ERR_DEVICE_NOT_READY : EngrossOne(w, ids[i], known, &tails);
            if (rc != OK)
                return rc;
        }
    }
    return OK;
}

/* Whether a Tally still waits for the tails. */
static bool EngrossStillOwed(void)
{
    return BinderEngrossOwed() && LooseLeaves() > 0;
}

/* What a walk of the service left, said when it differs from the last said:
 * the Tally waits for each of those tails. */
static void EngrossWalkSaid(const EngrossWalkState* w)
{
    if (w->refused == s_said.refused && w->first_fid == s_said.first_fid && w->first == s_said.first)
        return;
    __atomic_store_n(&s_said.boot, w->boot, __ATOMIC_RELAXED);
    __atomic_store_n(&s_said.refused, w->refused, __ATOMIC_RELAXED);
    s_said.first_fid = w->first_fid;
    s_said.first     = w->first;
    if (w->refused == 0)
        return;
    kprintf("[TagFS] %u file(s) keep their tail in Slips that would not be laid in a block — file %llu's first "
            "(error %u) — and a Tally waits for each of them: a change to the file lets it be tried again\n",
            w->refused, (unsigned long long)w->first_fid, (unsigned)w->first);
}

/*
 * A turn of the service inside the volume's door: whether it is listed again.
 * A walk answers every ask made before it began (s_pass); a turn gives its
 * core back after ENGROSS_TURN tails or ENGROSS_LOOK files, and is listed
 * again while the walk has files left and a Tally still waits — and once a
 * walk ended, again only for an ask made since it began. A Helm it found held
 * asks again at its let-go (HelmTryNoted), and a cycle that settles asks
 * again at its DONE (BinderWeigh): the walk never waits on a look it took
 * once.
 */
static bool EngrossTurnInside(void)
{
    if (!EngrossStillOwed() || EngrossWalk(&s_walk, ENGROSS_TURN, ENGROSS_LOOK, true) != OK)
        return false;
    if (s_walk.from != UINT64_MAX)
        return EngrossStillOwed();
    EscheatTailsWalked(s_walk.act);
    EngrossWalkSaid(&s_walk);
    s_walk.from = 0;
    return EngrossStillOwed() && __atomic_load_n(&s_asked, __ATOMIC_ACQUIRE) != s_pass;
}

static void EngrossTurn(HelmWaiter* w)
{
    bool more = false;
    (void)w;
    if (s_walk.from == 0) {
        s_walk = (EngrossWalkState){ .from = 0, .first = OK, .act = EscheatTailsActing() };
        s_pass = __atomic_load_n(&s_asked, __ATOMIC_ACQUIRE);
    }
    if (tagfs_enter()) {
        more = EngrossTurnInside();
        tagfs_leave();
    }
    if (more) {
        HelmReady(&s_service);
        return;
    }
    uint64_t answered = s_pass;
    s_walk.from = 0;
    __atomic_store_n(&s_listed, 0u, __ATOMIC_RELEASE);
    /* The second look: an ask made while this listing ended found it listed
     * and listed nothing (EngrossAsked); the walk it begins answers it. */
    if (__atomic_load_n(&s_asked, __ATOMIC_ACQUIRE) != answered)
        EngrossListed();
}

static void EngrossListed(void)
{
    if (__atomic_exchange_n(&s_listed, 1u, __ATOMIC_ACQ_REL) == 0)
        HelmReady(&s_service);
}

void EngrossAsked(void)
{
    __atomic_add_fetch(&s_asked, 1u, __ATOMIC_ACQ_REL);
    EngrossListed();
}

error_t EngrossWhole(void)
{
    EngrossWalkState w  = { .from = 0, .first = OK, .act = EscheatTailsActing() };
    error_t          rc = OK;
    while (rc == OK && w.from != UINT64_MAX)
        rc = EngrossWalk(&w, UINT32_MAX, UINT32_MAX, false);
    if (rc == OK)
        EscheatTailsWalked(w.act);
    if (rc != OK || w.left == 0)
        return rc;
    if (w.refused)
        kprintf("[TagFS] %u file(s) keep their tail in Slips that would not be laid in a block — file %llu's "
                "first (error %u) — no Tally now\n", w.refused, (unsigned long long)w.first_fid, (unsigned)w.first);
    else
        kprintf("[TagFS] %u file(s) keep their tail in Slips: the holder of the Helm writes them, or the ribbon "
                "has no room yet — no Tally now\n", w.left);
    return w.refused ? w.first : ERR_BUSY;
}

error_t EngrossGuarded(uint64_t fid, const char* name, uint32_t add, uint32_t drop, InkReceipt* out)
{
    error_t          rc   = OK;
    TagFSFileHandle* h    = EngrossOpen(fid, &rc);
    HelmWaiter       here;
    bool             laid = false;
    if (!h)
        return rc;
    rc = HelmTakeNow(h, &here);
    if (rc == OK) {
        rc = HelmEngross(h, &laid);     /* ERR_WOULD_BLOCK the caller's to wait out, as a tag's own Ink's */
        if (rc == OK)
            rc = InkFieldsLaid(fid, name, add, drop, INK_WARRANTED, out);
        HelmLetGo(h->ofe, &here);
    }
    tagfs_close(h);
    return rc;
}

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
void EngrossInked(uint64_t gen)
{
    __atomic_store_n(&s_inked, gen, __ATOMIC_RELEASE);
}

bool EngrossCarriedBy(uint64_t target)
{
    uint64_t gen = __atomic_load_n(&s_inked, __ATOMIC_ACQUIRE);
    return gen != 0 && gen <= target;
}
#endif
