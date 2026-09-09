#ifndef ADDR_WAIT_H
#define ADDR_WAIT_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
/* Do NOT include process.h here — process.h includes addr_wait.h, so a
 * full include would be circular.  We only need the pointer type. */
struct process_t;

/* Two-level hash table keyed by (address space, user VA).
 * 256 L1 pointers, each pointing to a lazily-allocated slab of 256
 * AddrWaitBuckets. Bucket index = AddrWaitIndex(space, va) & 0xFFFF;
 * hi = index >> 8  selects L1 slab;
 * lo = index & 0xFF selects bucket within the slab.
 *
 * The key is the word's address in its own home — the cabin's address space
 * (identified by its vmm context) and the virtual address the parker gave —
 * never the physical page behind it. It used to be the physical address, and
 * that keyed the wait to something the machine may move: a page migrated
 * from under a parker (MCE poison migration, CoW, a Bay re-open) filed the
 * waker under a different bucket and the wake could never arrive; boxlib and
 * boxcxx each held a 100 ms clock over every park to cover for it. Every
 * parker in the tree waits inside its own cabin — strands share one address
 * space, and nothing waits atomically across cabins through Bay — so the
 * (space, va) pair names exactly what the waker will name, whatever the
 * physical page does. The value itself is still read through the page (the
 * park's pre-check and recheck, Nightwatch's peek); only the KEY moved. */

#define ADDR_WAIT_L1_SIZE  256u
#define ADDR_WAIT_L2_SIZE  256u

typedef struct AddrWaitEntry {
    struct AddrWaitEntry *next;
    struct AddrWaitEntry *prev;
    uintptr_t             space;     /* the key's first half: the cabin's vmm context */
    /* The wake's own list. A wake claims every waiter on its address under
     * one hold of the bucket lock and delivers outside it; a claimed entry is
     * already off the chain, and this link is how it rides to its delivery
     * — no tray of N entries between the claim and the push, so there is no
     * N. Written only by the claimer, between the claim and the push. */
    struct AddrWaitEntry *claimed_next;
    struct process_t     *proc;
    uintptr_t             phys_addr; /* where the word was when the park began:
                                      * Nightwatch reads the word through it. Not
                                      * part of the key. */
    /* Recorded for Nightwatch. Without these a parked entry cannot be told
     * apart from a stalled one:
     *   user_va  — the key's second half, and what the evidence names.
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
    uint64_t              fire_at; /* the deadline, in scheduler ticks; 0 when the
                                    * park has none. The deadline's delivery judges
                                    * by THIS, under the bucket lock: a park whose
                                    * deadline has passed and that is still linked
                                    * is owed ERR_TIMEOUT, whichever fire woke the
                                    * deliverer — and a re-park with a deadline still
                                    * ahead is left alone. No token to pass, none to
                                    * go stale. */
    uint32_t              seq;    /* bumped on every link — identifies THIS park
                                   * to Nightwatch (a strand woken and parked again
                                   * is a different suspect). */
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

/* Get-or-create the bucket for a word named by (address space, user VA).
 * Returns NULL on OOM. */
AddrWaitBucket *AddrWaitGetBucket(uintptr_t space, uint64_t user_va);

/* Link entry into its bucket (caller holds bucket->lock). */
void AddrWaitLink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

/* Unlink entry from its bucket (caller holds bucket->lock). */
void AddrWaitUnlink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

/* Safe unlink: if entry->linked, recomputes its bucket, takes the lock,
 * unlinks, and unlocks.  No-op if not linked.  Safe to call twice. */
void AddrWaitUnlinkIfLinked(AddrWaitEntry *entry);

/* The claim itself, for a caller already holding the entry's bucket lock (the
 * wake loop walks a whole chain under it). Marks the entry done, unlinks it,
 * and marks the parked strand's chit DUE: from this instant the answer is
 * determined and the claimer owes its delivery. Every claim — a wake, a
 * deadline, the parker's own recheck — goes through here, so no claimer can
 * forget the chit. Returns false if somebody else already claimed. */
bool AddrWaitClaimLocked(AddrWaitBucket *bucket, AddrWaitEntry *entry);

/* Atomically claim a waiter: if it is still linked and not yet done, set
 * done=1 AND unlink it (all under the bucket lock), then return true. Exactly
 * one caller wins; that caller OWNS the wake and must deliver exactly one
 * completion Result to entry->proc. Every other caller gets false. This is the
 * single arbitration point shared by addr_wake and the parker's own lost-wakeup
 * recheck — it makes "who delivers the wake Result" race-free. */
bool AddrWaitClaim(AddrWaitEntry *entry);

/* The deadline's claim: succeeds only for a park that is still linked, has a
 * deadline, and whose deadline is at or before `now` (scheduler ticks). Judged
 * under the bucket lock from the entry's own state, so it needs no token from
 * the fire that woke the deliverer: a stale fire finds a park with a deadline
 * still ahead (or none) and claims nothing; a fire that landed between the
 * link and the sleep finds a park whose deadline has passed and claims it.
 * On success `*cookie_out` is the park's cloakroom token, read under the same
 * lock. Exactly one of {addr_wake, the deadline, the parker's recheck} wins. */
bool AddrWaitClaimDue(AddrWaitEntry *entry, uint64_t now, uint32_t *cookie_out);

#endif /* ADDR_WAIT_H */
