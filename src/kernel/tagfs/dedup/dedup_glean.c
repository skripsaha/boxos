#include "dedup_internal.h"
#include "../settle/dog_ear.h"
#include "../settle/settle.h"
#include "../integrity/gloss.h"
#include "../helm/helm.h"
#include "volume_book.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "tagfs_volume.h"

/*
 * Dedup's door (dedup.h): what a Tally's DONE hands over, and the service of
 * the Helm the pass runs its turns on (dedup_pass.c).
 *
 * ‼ THE CANDIDATES ARE TAKEN BEFORE THE GLOSS LETS THEM GO. The blocks a Tally
 * took in hold the digests their writes gave them, which the Gloss alone holds
 * until that Tally's DONE turns the reads to its DIGESTS and lets them go
 * (settle/binder_done.c, GlossCaptured): so the DONE hands them over first, a
 * page of digests at a time out of the Gloss, into memory the heap gives
 * before any lock — and the walk never reads the leaves the Tally just laid to
 * learn what it wrote. A block the Gloss names no digest for is not weighed.
 *
 * The batches wait in order under a leaf lock of their own, which holds
 * nothing but their list and the stand's sum; the service takes them all at
 * a pass's beginning.
 */

#define DEDUP_GLOSS_PAGE  512u     /* digests one copy out of the Gloss takes: a page of them */

static void DedupTurn(HelmWaiter* w);

#ifdef CONFIG_VOLUME_TESTS
static DedupSaid         s_stand;              /* under dedup_glean.lock */
#endif

static void DedupListed(TagFSVolume* vol)
{
    if (__atomic_exchange_n(&vol->dedup_glean.listed, 1u, __ATOMIC_ACQ_REL) == 0)
        HelmReady(vol, &vol->dedup_glean.service);
}

static bool DedupPendingAny(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->dedup_glean.pending, __ATOMIC_ACQUIRE) != NULL;
}

/* A turn of the service: one step of the pass inside the volume's door, and
 * listed again while it has more — or, ended, once more for a batch handed
 * over while it ran, unless it waits for the ribbon's room (DedupWeigh); and
 * once more for room that came while it was listed, whose cycle's DONE found
 * the listing taken. A turn that finds the door shut lists nothing: the next
 * mount lets go of whatever waits (DedupLetGo). */
static void DedupTurn(HelmWaiter* w)
{
    TagFSVolume* vol = container_of(w, TagFSVolume, dedup_glean.service);
    bool more = false, entered = tagfs_enter(vol);
    if (entered) {
        more = DedupPassTurn(vol);
        tagfs_leave(vol);
    }
    if (more) {
        HelmReady(vol, &vol->dedup_glean.service);
        return;
    }
    __atomic_store_n(&vol->dedup_glean.listed, 0u, __ATOMIC_RELEASE);
    if (entered && (__atomic_exchange_n(&vol->dedup_glean.again, 0u, __ATOMIC_ACQ_REL) ||
                    (!__atomic_load_n(&vol->dedup_glean.room, __ATOMIC_ACQUIRE) && DedupPendingAny(vol))))
        DedupListed(vol);
}

void DedupBatchLetGo(DedupBatch* b)
{
    if (!b)
        return;
    kfree(b->entry);
    kfree(b->gone);
    kfree(b);
}

/* `count` blocks from `start`, their digests out of the Gloss a page at a
 * time (`le` room for one), into the batch in block order — a block the Gloss
 * names no digest for left out. */
static void DedupBatchRun(TagFSVolume* vol, DedupBatch* b, uint64_t start, uint64_t count, uint8_t* le)
{
    for (uint64_t at = 0; at < count;) {
        uint64_t n   = count - at < DEDUP_GLOSS_PAGE ? count - at : DEDUP_GLOSS_PAGE;
        bool     all = GlossCopy(vol, start + at, n, le);
        for (uint64_t k = 0; k < n; k++) {
            uint64_t digest = 0;
            if (all)
                digest = TallyDigestAt(le, k);
            else if (!GlossFind(vol, start + at + k, &digest))
                continue;
            b->entry[b->n++] = (CommonplaceEntry){ .digest = digest, .block = start + at + k };
        }
        at += n;
    }
}

/* The batch of the `blocks` blocks of `born` (`nb` runs) and of `gone` (`ng`
 * runs): NULL with no memory for it. */
static DedupBatch* DedupBatchMake(TagFSVolume* vol, const BinderGround* born, uint64_t nb, uint64_t blocks, const BinderGround* gone,
                                  uint64_t ng)
{
    if (blocks > UINT64_MAX / sizeof(CommonplaceEntry) || ng > UINT64_MAX / sizeof(BinderGround))
        return NULL;
    DedupBatch* b  = kmalloc(sizeof(DedupBatch));
    uint8_t*    le = kmalloc(DEDUP_GLOSS_PAGE * 8u);
    if (b) {
        memset(b, 0, sizeof(*b));
        b->entry = blocks ? kmalloc(sizeof(CommonplaceEntry) * blocks) : NULL;
        b->gone  = ng ? kmalloc(sizeof(BinderGround) * ng) : NULL;
    }
    if (!b || !le || (blocks && !b->entry) || (ng && !b->gone)) {
        kfree(le);
        DedupBatchLetGo(b);
        return NULL;
    }
    if (ng)
        memcpy(b->gone, gone, sizeof(BinderGround) * ng);
    b->ngone = ng;
    for (uint64_t i = 0; i < nb; i++)
        DedupBatchRun(vol, b, born[i].start, born[i].count, le);
    kfree(le);
    return b;
}

/*
 * ‼ THE WALK IS PAID FOR BY WHAT WAS WRITTEN. The pass may read as many Sheets
 * of the book's DIGESTS as the Tally laid and its blocks take on the medium —
 * a block is TAGFS_BLOCK_SIZE / BOOK_SECTOR_BYTES Sheets — so finding a copy
 * costs at most the I/O the copy itself cost, and the walk goes round a book
 * once for every DIGESTS-worth of data written: what Commonplace holds while
 * it waits for that is about a 512th of the volume's blocks, whatever the rate.
 */
void DedupGleanAt(TagFSVolume* vol, uint64_t tally, uint64_t sheets)
{
    uint64_t nb = 0, ng = 0, blocks = 0;
    if (__atomic_load_n(&vol->dedup_glean.held, __ATOMIC_ACQUIRE))
        return;
    if (DogEarTakenLost(vol)) {
        kprintf("[TagFS] Dedup: DogEar lost the ground Tally %llu took in to a heap that refused it — the pass "
                "after it weighs none of it\n", (unsigned long long)tally);
        return;
    }
    const BinderGround* born = DogEarTakenGround(vol, false, &nb);
    const BinderGround* gone = DogEarTakenGround(vol, true, &ng);
    for (uint64_t i = 0; i < nb; i++)
        blocks += born[i].count;
    if (blocks == 0 && ng == 0 && __atomic_load_n(&vol->dedup_glean.holding, __ATOMIC_ACQUIRE) == 0)
        return;
    DedupBatch* b = DedupBatchMake(vol, born, nb, blocks, gone, ng);
    if (!b) {
        kprintf("[TagFS] Dedup: no memory to weigh the %llu block(s) Tally %llu took in — none of them is "
                "weighed\n", (unsigned long long)blocks, (unsigned long long)tally);
        return;
    }
    b->tally  = tally;
    b->budget = b->n * (TAGFS_BLOCK_SIZE / BOOK_SECTOR_BYTES) + sheets;
    TagFSLockTake(&vol->dedup_glean.lock);
    DedupBatch** tail = &vol->dedup_glean.pending;
    while (*tail)
        tail = &(*tail)->next;
    *tail = b;
    TagFSLockDrop(&vol->dedup_glean.lock);
    DedupListed(vol);
}

DedupBatch* DedupPendingTake(TagFSVolume* vol)
{
    TagFSLockTake(&vol->dedup_glean.lock);
    DedupBatch* b = vol->dedup_glean.pending;
    vol->dedup_glean.pending     = NULL;
    TagFSLockDrop(&vol->dedup_glean.lock);
    return b;
}

bool DedupHeld(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->dedup_glean.held, __ATOMIC_ACQUIRE);
}

void DedupRoomWait(TagFSVolume* vol)
{
    __atomic_store_n(&vol->dedup_glean.room, 1u, __ATOMIC_RELEASE);
    SettleDemand(vol, SETTLE_DEMAND_ROOM);
}

void DedupHolding(TagFSVolume* vol, uint64_t entries)
{
    __atomic_store_n(&vol->dedup_glean.holding, entries, __ATOMIC_RELEASE);
}

void DedupWeigh(TagFSVolume* vol)
{
    if (__atomic_exchange_n(&vol->dedup_glean.room, 0u, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&vol->dedup_glean.again, 1u, __ATOMIC_RELEASE);
        DedupListed(vol);
    }
}

bool DedupHold(TagFSVolume* vol, bool held)
{
    bool was = __atomic_exchange_n(&vol->dedup_glean.held, held, __ATOMIC_ACQ_REL);
    if (held && !was)
        DedupListed(vol);      /* the turn lets go of what Commonplace holds */
    return was;
}

void DedupLetGo(TagFSVolume* vol)
{
    DedupBatch* b = DedupPendingTake(vol);
    while (b) {
        DedupBatch* next = b->next;
        DedupBatchLetGo(b);
        b = next;
    }
    __atomic_store_n(&vol->dedup_glean.room, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->dedup_glean.again, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->dedup_glean.holding, 0u, __ATOMIC_RELEASE);
    DedupPassLetGo(vol);
}

#ifdef CONFIG_VOLUME_TESTS
#define DEDUP_STAND_MS  60000u

void DedupPassSaid(TagFSVolume* vol, const DedupSaid* said)
{
    TagFSLockTake(&vol->dedup_glean.lock);
    s_stand.passes++;
    s_stand.weighed += said->weighed;
    s_stand.among   += said->among;
    s_stand.book    += said->book;
    s_stand.laid    += said->laid;
    s_stand.refused += said->refused;
    s_stand.passed  += said->passed;
    s_stand.scant   += said->scant;
    TagFSLockDrop(&vol->dedup_glean.lock);
}

static bool DedupBusy(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->dedup_glean.listed, __ATOMIC_ACQUIRE) || __atomic_load_n(&vol->dedup_glean.room, __ATOMIC_ACQUIRE) ||
           DedupPendingAny(vol);
}

bool DedupStandPassed(TagFSVolume* vol, DedupSaid* out)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(DEDUP_STAND_MS);
    while (DedupBusy(vol) && rdtsc() < bound) {
        if (__atomic_load_n(&vol->dedup_glean.room, __ATOMIC_ACQUIRE))
            (void)TagFSSettle(vol);
        HelmRunReady(vol);
        cpu_pause();
    }
    TagFSLockTake(&vol->dedup_glean.lock);
    *out = s_stand;
    memset(&s_stand, 0, sizeof(s_stand));
    TagFSLockDrop(&vol->dedup_glean.lock);
    return !DedupBusy(vol);
}
#endif

/* The volume's dedup_glean part as it begins: what was not nothing. */
void DedupGleanVolumeBegin(TagFSVolume* vol)
{
    vol->dedup_glean.lock = (TagFSLock)TAGFS_LOCK_INIT("dedup", LOCK_RANK_LEAF);
    vol->dedup_glean.service = (HelmWaiter){ .run = DedupTurn };
}
