#ifndef CABIN_H
#define CABIN_H

#include "ktypes.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"

/*
 * cabin_t — shared address-space object.
 *
 * One cabin owns the vmm_context (CR3), IPC rings, MemTag state, ASLR
 * bases, and all Bay/Brook/Touch claim lists.  In P1 there is always
 * exactly one strand per cabin (strand_count==1).  P4 will add ≥2 strands.
 *
 * Lock ordering inside cabin: bay_lock / brook_lock / subs_lock are
 * independent per-cabin locks.  No global lock is taken by cabin_destroy.
 *
 * strand_count != process_t.ref_count — they track different lifetimes.
 */

/* Retired (grown-past) tag-overflow buffers. On growth the old buffer is
 * NOT freed — a lock-free reader (process_has_tag_id on the hot publish
 * path) may still hold the pointer — it is chained here and freed only in
 * cabin_destroy. Reallocs are rare and bounded (~7/cabin), so the retained
 * memory is negligible. */
typedef struct TagOverflowRetired
{
    uint16_t                  *buf;
    struct TagOverflowRetired *next;
} TagOverflowRetired;

typedef struct cabin_t
{
    vmm_context_t *vmm;

    uint64_t cabin_info_phys;
    /* The MAIN strand's IPC rings (fixed low VAs 0x2000/0x3000/0x5000),
     * allocated + mapped by vmm_create_cabin and reclaimed by
     * vmm_destroy_context at cabin teardown. P5a: the main strand's
     * process_t.{pocket,result,touch}_ring_phys ALIAS these; each spawned
     * strand instead carries its OWN per-strand rings carved from the Hammock
     * window (strand_rings.c). kring.c / touch_ring.c route by the per-strand
     * process_t fields, never these. */
    uint64_t pocket_ring_phys;
    uint64_t result_ring_phys;
    uint64_t touch_ring_phys;

    uint64_t tag_bits;
    uint16_t *tag_overflow_ids;
    uint16_t  tag_overflow_count;
    uint16_t  tag_overflow_capacity;
    TagOverflowRetired *tag_overflow_retired;  /* old buffers, freed at cabin_destroy */

    /* MemTag per-cabin capability bitmask — 1024 bits (tag_ids 0..1023). */
    uint64_t active_memtags[16];

    uintptr_t code_start;
    size_t    code_size;

    /* ASLR: per-cabin randomized addresses (set at creation time). */
    uint64_t aslr_heap_base;
    uint64_t aslr_stack_top;
    uint64_t aslr_buf_heap_base;
    uint64_t buf_heap_next;

    uint32_t spawner_pid;

    /* Touch subscriber list. */
    void       *subs_head;
    spinlock_t  subs_lock;

    /* Bay claim list. */
    void       *bay_claims_head;
    spinlock_t  bay_lock;
    uint64_t    bay_va_next;
    uint16_t    tme_keyids_held;

    /* Brook claim list. */
    void       *brook_claims_head;
    spinlock_t  brook_lock;
    uint64_t    brook_va_next;

    /* Hammock cursor — bump-allocates one fixed-size VA slot per spawned
     * strand for its user stack + CET shadow stack + per-strand IPC rings +
     * StrandInfo TLS block (see cabin_layout.h + strand_rings.c). The main
     * strand does NOT consume a slot. Guarded by hammock_lock so concurrent
     * strand_spawn from sibling strands never hand out the same VA. Mirrors
     * bay_va_next / brook_va_next. */
    uint64_t    hammock_va_next;
    spinlock_t  hammock_lock;

    /*
     * strand_count: number of strands currently attached to this cabin.
     * In P1 always 1.  cabin_ref_dec calls cabin_destroy when it hits 0.
     * This is NOT the same as process_t.ref_count (which tracks external
     * K-Core / pointer refs to the strand).
     */
    atomic_u32_t strand_count;
} cabin_t;

_Static_assert(sizeof(cabin_t) < 4096, "cabin_t must fit in one page");

/*
 * cabin_create — allocate and initialise a cabin_t.
 *
 * Calls vmm_create_cabin, initialises all IPC rings, applies MemTag to
 * ring pages, interns the initial tag string, generates ASLR offsets,
 * and seeds Bay/Brook/Touch heads.  Sets strand_count = 1.
 *
 * Returns NULL on any allocation failure (caller must abort process_create).
 */
cabin_t *cabin_create(uint32_t pid, const char *tags);

/*
 * cabin_destroy — release all shared resources owned by this cabin.
 *
 * Calls TouchFinalizeProcess, vmm_destroy_context, frees tag_overflow_ids.
 * Takes NO global locks.  Must be called only when strand_count == 0.
 */
void cabin_destroy(cabin_t *cabin);

/* Reference counting on strand_count. */
void cabin_ref_inc(cabin_t *cabin);
/* Decrements strand_count; calls cabin_destroy when it reaches 0. */
void cabin_ref_dec(cabin_t *cabin);

#endif /* CABIN_H */
