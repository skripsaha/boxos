#ifndef ADDR_WAIT_H
#define ADDR_WAIT_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
/* Do NOT include process.h here — process.h includes addr_wait.h, so a
 * full include would be circular.  We only need the pointer type. */
struct process_t;

/* Two-level hash table keyed by physical address.
 * 256 L1 pointers, each pointing to a lazily-allocated slab of 256
 * AddrWaitBuckets. Bucket index = (phys >> 3) & 0xFFFF;
 * hi = index >> 8  selects L1 slab;
 * lo = index & 0xFF selects bucket within the slab.
 *
 * REAL-HW CAVEAT: parker and waker must resolve the SAME physical address
 * for the same user VA. That holds for strands sharing a cabin (one PTE)
 * and for stable .bss/.data/heap pages. But a VA whose backing page can be
 * remapped underneath it (CoW fork, MCE poison migration, Bay re-open)
 * could hash parker and waker to different buckets and lose the wake until
 * the park timeout fires. Callers using addr_park/addr_wake as a futex
 * (std::atomic::wait) must keep the word on a pinned, non-migrating page. */

#define ADDR_WAIT_L1_SIZE  256u
#define ADDR_WAIT_L2_SIZE  256u

typedef struct AddrWaitEntry {
    struct AddrWaitEntry *next;
    struct AddrWaitEntry *prev;
    struct process_t     *proc;
    uintptr_t             phys_addr;
    /* Recorded for Nightwatch. Without these three a parked entry cannot be
     * told apart from a permanently unreachable one:
     *   user_va  — re-resolving it and getting a DIFFERENT phys proves the
     *              backing page moved under the parker, so a waker hashes to
     *              another bucket and this wake can never arrive. That is the
     *              REAL-HW CAVEAT above; until now the kernel kept no evidence
     *              that it had actually happened.
     *   expected — the value the parker waits to see change. If it HAS changed
     *              while this entry is still linked — and is still so a full
     *              look later, for the same seq — a wake was owed and never
     *              delivered. Not at an instant: the waker stores before it
     *              asks for the wake, so a look that lands between the two
     *              sees exactly this on a healthy machine (nightwatch.c).
     *   timed    — a park with a deadline recovers by itself, so it is not a
     *              stall even when everything around it is asleep. */
    uint64_t              user_va;
    uint64_t              expected;
    uint32_t              seq;    /* bumped on every link — identifies THIS park.
                                   * A park-timeout snapshots it at arm time and
                                   * only claims if it still matches, so a stale
                                   * timeout (its park was woken early and the
                                   * strand re-parked, reusing this entry) can
                                   * never inject ERR_TIMEOUT into the new wait. */
    uint32_t              submit_cookie;
                                  /* the park submit's cloakroom token
                                   * (OpContext.submit_cookie), echoed into the
                                   * wake/timeout Result's context high bits so
                                   * the parked strand's paired wait adopts ONLY
                                   * its own completion — a stray reply on its
                                   * ring can no longer read as a spurious wake. */
    uint8_t               done;   /* 1 = waker claimed this entry */
    uint8_t               linked; /* 1 = currently in a bucket chain */
    uint8_t               timed;  /* 1 = a park deadline was armed for this park */
} AddrWaitEntry;

typedef struct {
    spinlock_t    lock;
    AddrWaitEntry *head;
} AddrWaitBucket;

typedef struct {
    AddrWaitBucket buckets[ADDR_WAIT_L2_SIZE];
} AddrWaitSlab;

/* Global registry — L1 array of pointers to lazily-allocated slabs. */
typedef struct {
    AddrWaitSlab *slabs[ADDR_WAIT_L1_SIZE];  /* NULL until first waiter in that slab */
} AddrWaitTable;

void AddrWaitTableInit(void);

/* Get-or-create the bucket for a physical address. Returns NULL on OOM. */
AddrWaitBucket *AddrWaitGetBucket(uintptr_t phys_addr);

/* Link entry into its bucket (caller holds bucket->lock). */
void AddrWaitLink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

/* Unlink entry from its bucket (caller holds bucket->lock). */
void AddrWaitUnlink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

/* Safe unlink: if entry->linked, recomputes its bucket, takes the lock,
 * unlinks, and unlocks.  No-op if not linked.  Safe to call twice. */
void AddrWaitUnlinkIfLinked(AddrWaitEntry *entry);

/* Atomically claim a waiter: if it is still linked and not yet done, set
 * done=1 AND unlink it (all under the bucket lock), then return true. Exactly
 * one caller wins; that caller OWNS the wake and must deliver exactly one
 * completion Result to entry->proc. Every other caller gets false. This is the
 * single arbitration point shared by addr_wake and the parker's own lost-wakeup
 * recheck — it makes "who delivers the wake Result" race-free. */
bool AddrWaitClaim(AddrWaitEntry *entry);

/* Like AddrWaitClaim, but only succeeds if entry->seq still equals `seq` (the
 * value snapshotted when the park was armed). A stale park-timeout — whose park
 * was already woken early and whose strand re-parked, bumping seq — fails the
 * match and claims nothing. Exactly one of {addr_wake, the matching timeout,
 * the parker's recheck} wins delivery. */
bool AddrWaitClaimSeq(AddrWaitEntry *entry, uint32_t seq);

#endif /* ADDR_WAIT_H */
