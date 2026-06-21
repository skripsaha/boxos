/*
 * addr_wait.c — two-level hash registry of parked strands by physical address.
 *
 * Bucket index = (phys >> 3) & 0xFFFF
 *   hi = index >> 8  → L1 slab index
 *   lo = index & 0xFF → bucket within slab
 *
 * Slabs are lazily kmalloc'd: the first waiter for a given hi-byte
 * allocates the 256-bucket slab and CAS-installs it; a concurrent loser
 * frees its allocation and uses the winner's slab. This mirrors the
 * pattern used by TouchBucket.
 */

#include "addr_wait.h"
#include "klib.h"
#include "atomics.h"

static AddrWaitTable g_addr_wait_table;

void AddrWaitTableInit(void)
{
    for (uint32_t i = 0; i < ADDR_WAIT_L1_SIZE; i++) {
        __atomic_store_n(&g_addr_wait_table.slabs[i], NULL, __ATOMIC_RELAXED);
    }
}

static AddrWaitSlab *get_or_create_slab(uint32_t hi)
{
    AddrWaitSlab *existing = __atomic_load_n(&g_addr_wait_table.slabs[hi],
                                              __ATOMIC_ACQUIRE);
    if (existing) return existing;

    AddrWaitSlab *slab = (AddrWaitSlab *)kmalloc(sizeof(AddrWaitSlab));
    if (!slab) return NULL;

    for (uint32_t i = 0; i < ADDR_WAIT_L2_SIZE; i++) {
        spinlock_init(&slab->buckets[i].lock);
        slab->buckets[i].head = NULL;
    }

    AddrWaitSlab *expected = NULL;
    if (!__atomic_compare_exchange_n(&g_addr_wait_table.slabs[hi],
                                     &expected, slab,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        kfree(slab);
        return expected;
    }
    return slab;
}

AddrWaitBucket *AddrWaitGetBucket(uintptr_t phys_addr)
{
    uint32_t index = (uint32_t)((phys_addr >> 3) & 0xFFFF);
    uint32_t hi    = index >> 8;
    uint32_t lo    = index & 0xFF;

    AddrWaitSlab *slab = get_or_create_slab(hi);
    if (!slab) return NULL;
    return &slab->buckets[lo];
}

void AddrWaitLink(AddrWaitBucket *bucket, AddrWaitEntry *entry)
{
    entry->prev   = NULL;
    entry->next   = bucket->head;
    if (bucket->head) bucket->head->prev = entry;
    bucket->head  = entry;
    entry->linked = 1;
}

void AddrWaitUnlink(AddrWaitBucket *bucket, AddrWaitEntry *entry)
{
    if (entry->prev) entry->prev->next = entry->next;
    else             bucket->head      = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    entry->next   = NULL;
    entry->prev   = NULL;
    entry->linked = 0;
}

void AddrWaitUnlinkIfLinked(AddrWaitEntry *entry)
{
    if (!entry->linked) return;
    AddrWaitBucket *bucket = AddrWaitGetBucket(entry->phys_addr);
    if (!bucket) return;
    spin_lock(&bucket->lock);
    if (entry->linked) AddrWaitUnlink(bucket, entry);
    spin_unlock(&bucket->lock);
}

bool AddrWaitClaim(AddrWaitEntry *entry)
{
    /* Cheap unlocked pre-check: an already-unlinked entry can never be claimed,
     * and avoiding the bucket lookup keeps the hot wake path lean. The locked
     * re-check below is authoritative. */
    if (!entry->linked) return false;
    AddrWaitBucket *bucket = AddrWaitGetBucket(entry->phys_addr);
    if (!bucket) return false;

    bool won = false;
    spin_lock(&bucket->lock);
    if (entry->linked && !entry->done) {
        entry->done = 1;            /* claim — exactly one winner */
        AddrWaitUnlink(bucket, entry);
        won = true;
    }
    spin_unlock(&bucket->lock);
    return won;
}
