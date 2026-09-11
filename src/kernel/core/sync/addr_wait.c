
#include "addr_wait.h"
#include "chit.h"
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

AddrWaitBucket *AddrWaitGetBucket(uintptr_t space, uint64_t user_va)
{
    uint32_t mixed = (uint32_t)((user_va >> 3) ^ ((space >> 4) * 0x9E3779B1u));
    uint32_t index = mixed & 0xFFFF;
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
    entry->seq++;
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
    AddrWaitBucket *bucket = AddrWaitGetBucket(entry->space, entry->user_va);
    if (!bucket) return;
    spin_lock(&bucket->lock);
    if (entry->linked) AddrWaitUnlink(bucket, entry);
    spin_unlock(&bucket->lock);
}

bool AddrWaitClaimLocked(AddrWaitBucket *bucket, AddrWaitEntry *entry)
{
    if (!entry->linked || entry->done) return false;
    entry->done = 1;
    AddrWaitUnlink(bucket, entry);
    ChitDue(entry->proc, entry->submit_cookie);
    return true;
}

bool AddrWaitClaim(AddrWaitEntry *entry)
{
    if (!entry->linked) return false;
    AddrWaitBucket *bucket = AddrWaitGetBucket(entry->space, entry->user_va);
    if (!bucket) return false;

    spin_lock(&bucket->lock);
    bool won = AddrWaitClaimLocked(bucket, entry);
    spin_unlock(&bucket->lock);
    return won;
}

bool AddrWaitClaimDue(AddrWaitEntry *entry, uint64_t now, uint32_t *cookie_out)
{
    if (!entry->linked) return false;
    AddrWaitBucket *bucket = AddrWaitGetBucket(entry->space, entry->user_va);
    if (!bucket) return false;

    spin_lock(&bucket->lock);
    bool due = entry->linked && entry->timed && now >= entry->fire_at;
    bool won = due && AddrWaitClaimLocked(bucket, entry);
    if (won) *cookie_out = entry->submit_cookie;
    spin_unlock(&bucket->lock);
    return won;
}