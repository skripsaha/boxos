#ifndef CABIN_H
#define CABIN_H

#include "ktypes.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"


typedef struct TagOverflowRetired
{
    uint16_t                  *buf;
    struct TagOverflowRetired *next;
} TagOverflowRetired;

typedef struct cabin_t
{
    vmm_context_t *vmm;

    uint64_t cabin_info_phys;
    uint64_t pocket_ring_phys;
    uint64_t result_ring_phys;
    uint64_t touch_ring_phys;

    uint64_t tag_bits;

    uint32_t auth_bits;

    uint16_t *tag_overflow_ids;
    uint16_t  tag_overflow_count;
    uint16_t  tag_overflow_capacity;
    TagOverflowRetired *tag_overflow_retired;

    uint64_t active_memtags[16];

    uintptr_t code_start;
    size_t    code_size;

    uint64_t aslr_heap_base;
    uint64_t aslr_stack_top;
    uint64_t aslr_buf_heap_base;
    uint64_t buf_heap_next;

    uint32_t spawner_pid;
    uint32_t spawner_gen;

    void       *subs_head;
    spinlock_t  subs_lock;

    void       *bay_claims_head;
    spinlock_t  bay_lock;
    uint64_t    bay_va_next;
    uint16_t    tme_keyids_held;

    void       *brook_claims_head;
    spinlock_t  brook_lock;
    uint64_t    brook_va_next;

    uint64_t    hammock_va_next;
    spinlock_t  hammock_lock;

    atomic_u32_t strand_count;
} cabin_t;

_Static_assert(sizeof(cabin_t) < 4096, "cabin_t must fit in one page");

cabin_t *cabin_create(uint32_t pid, const char *tags);

void cabin_destroy(cabin_t *cabin);

void cabin_ref_inc(cabin_t *cabin);
void cabin_ref_dec(cabin_t *cabin);

#endif