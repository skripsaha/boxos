#ifndef ADDR_WAIT_H
#define ADDR_WAIT_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
struct process_t;


#define ADDR_WAIT_L1_SIZE  256u
#define ADDR_WAIT_L2_SIZE  256u

typedef struct AddrWaitEntry {
    struct AddrWaitEntry *next;
    struct AddrWaitEntry *prev;
    uintptr_t             space;
    struct AddrWaitEntry *claimed_next;
    struct process_t     *proc;
    uintptr_t             phys_addr;
    uint64_t              user_va;
    uint64_t              expected;
    uint64_t              fire_at;
    uint32_t              seq;
    uint32_t              submit_cookie;
    uint8_t               done;
    uint8_t               linked;
    uint8_t               timed;
} AddrWaitEntry;

typedef struct {
    spinlock_t    lock;
    AddrWaitEntry *head;
} AddrWaitBucket;

typedef struct {
    AddrWaitBucket buckets[ADDR_WAIT_L2_SIZE];
} AddrWaitSlab;

typedef struct {
    AddrWaitSlab *slabs[ADDR_WAIT_L1_SIZE];
} AddrWaitTable;

void AddrWaitTableInit(void);

AddrWaitBucket *AddrWaitGetBucket(uintptr_t space, uint64_t user_va);

void AddrWaitLink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

void AddrWaitUnlink(AddrWaitBucket *bucket, AddrWaitEntry *entry);

void AddrWaitUnlinkIfLinked(AddrWaitEntry *entry);

bool AddrWaitClaimLocked(AddrWaitBucket *bucket, AddrWaitEntry *entry);

bool AddrWaitClaim(AddrWaitEntry *entry);

bool AddrWaitClaimDue(AddrWaitEntry *entry, uint64_t now, uint32_t *cookie_out);

#endif