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
 * lo = index & 0xFF selects bucket within the slab. */

#define ADDR_WAIT_L1_SIZE  256u
#define ADDR_WAIT_L2_SIZE  256u

typedef struct AddrWaitEntry {
    struct AddrWaitEntry *next;
    struct AddrWaitEntry *prev;
    struct process_t     *proc;
    uintptr_t             phys_addr;
    uint8_t               done;   /* 1 = waker claimed this entry */
    uint8_t               linked; /* 1 = currently in a bucket chain */
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

#endif /* ADDR_WAIT_H */
