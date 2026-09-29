#ifndef KHEAP_H
#define KHEAP_H

#include "ktypes.h"

/*
 * The kernel heap: who an allocation is for, and what is kept back.
 *
 * kmalloc and kfree (klib.h) are the whole of the heap most code sees. This is
 * the part the rest of the kernel tells it, and the part it answers:
 *
 *   - WHOM an allocation is charged to. Everything a cabin can make the kernel
 *     hold — the names it interns, the records of the files it makes, the
 *     structures of the processes it spawns — is charged to the class of that
 *     cabin, from the moment the guide starts serving one of its pockets to
 *     the moment it is done. A cabin class may not hold more of the kernel
 *     heap than the machine has left free once it is served; past that it is
 *     refused, out loud, with the numbers. What the kernel does on its own
 *     account — interrupts, continuations, boot — is charged to the kernel,
 *     which no rule limits but the machine.
 *
 *   - WHAT IS KEPT BACK. The kernel's own paths that must finish whatever the
 *     machine is short of — the passes that write the volume (a Scribe's
 *     holder: the sync with its registry, pool, file table, integrity and
 *     bitmap, the registry's pass and the SpareStamp inside it, the Ledger's
 *     pass) and the integrity map's pages — are served from a reserve that
 *     nothing else can take from. The reserve holds what those paths have held
 *     at once: it grows by what it was short, the first time it is, and never
 *     gives memory back. Chit, which answers every waiter, allocates nothing
 *     and needs no share of it.
 */

/* The class an allocation is charged to. A cabin's is the highest authority
 * key it carries (auth_tags.h): god, then system (or bypass, which authorises
 * as system does), then utility, then app; a cabin with none is OTHER. */
typedef enum {
    KHEAP_CHARGE_KERNEL = 0,
    KHEAP_CHARGE_GOD,
    KHEAP_CHARGE_SYSTEM,
    KHEAP_CHARGE_UTILITY,
    KHEAP_CHARGE_APP,
    KHEAP_CHARGE_OTHER,
    KHEAP_CHARGE_COUNT
} KHeapCharge;

/* What this core charges its allocations to, and whether it is on one of the
 * kernel's own paths. One per core: the guide serves a pocket without giving
 * its core away (K-Cores run no processes; a single core serves a syscall with
 * interrupts off), and a Scribe's holder is never switched away from. */
typedef struct {
    uint8_t  charge;     /* KHeapCharge */
    uint8_t  reserve;    /* depth of KHeapReserveOpen on this core */
    uint32_t pid;        /* whom the charge is for, as a refusal names it */
} KHeapScope;

/* Charge this core's allocations to `charge` for `pid`, off any reserve path,
 * until KHeapScopeLeave puts back the scope this returns. Scopes nest. */
KHeapScope KHeapScopeEnter(KHeapCharge charge, uint32_t pid);
void       KHeapScopeLeave(KHeapScope before);

/* This core is on one of the kernel's own paths (above) until the matching
 * Close: what it allocates is served from the reserve first. */
void KHeapReserveOpen(void);
void KHeapReserveClose(void);

/* What a class holds of the kernel heap — its pool blocks, its large pieces
 * and its slab pages — and how many of its allocations were ever refused. */
size_t   KHeapChargeHeld(KHeapCharge charge);
uint64_t KHeapChargeRefusals(KHeapCharge charge);

/* The heap's shape, taken under its lock. */
typedef struct {
    uint32_t regions;          /* ordinary regions, the boot one included */
    size_t   region_bytes;     /* what they take from the machine */
    size_t   pool_live;        /* bytes of their live blocks, headers included */
    uint32_t large;            /* large pieces standing */
    size_t   large_bytes;      /* the pages they take */
    uint32_t reserve_regions;
    size_t   reserve_bytes;    /* what the reserve takes from the machine */
    size_t   reserve_live;     /* what the kernel's own paths hold of it now */
} KHeapShape;

void KHeapShapeNow(KHeapShape *out);

#endif /* KHEAP_H */
