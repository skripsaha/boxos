#ifndef PMM_H
#define PMM_H

#include "klib.h"
#include "error.h"
#include "buddy.h"
#include "kernel_config.h"

#define PMM_PAGE_SIZE       4096
#define PMM_BITMAP_ALIGN    8

#define PHYS_TAG_DMA32   (1ULL << 0)
#define PHYS_TAG_USER    (1ULL << 1)
#define PHYS_TAG_HIGH    (1ULL << 2)

typedef enum {
    PMM_FRAME_FREE = 0,
    PMM_FRAME_USED,
    PMM_FRAME_RESERVED,
    PMM_FRAME_KERNEL,
    PMM_FRAME_BAD
} pmm_frame_state_t;

error_t pmm_init(void);
void    pmm_free(void* addr, size_t pages);

void    PmmHoldBootServicesMemory(void);
void    PmmReleaseBootServicesMemory(void);

size_t   pmm_total_pages(void);
size_t   pmm_max_alloc_pages(void);
size_t   pmm_free_pages(void);
size_t   pmm_used_pages(void);
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_total_ram_bytes(void);
void     pmm_dump_stats(void);

void pmm_print_memory_map(void);

error_t pmm_set_maxphyaddr(uint8_t maxphyaddr);
uint8_t pmm_get_maxphyaddr(void);

uint64_t pmm_get_mem_end(void);

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size);

uint32_t pmm_phys_domain(uintptr_t phys);

void pmm_log_numa_topology(void);

void* pmm_alloc_in_domain(size_t pages, uint32_t domain);

size_t pmm_pages_in_domain(uint32_t domain);

void pmm_activate_pull_map(void);
void pmm_test_high_memory(void);

void pmm_set_poisoned(uintptr_t phys);
bool pmm_is_poisoned(uintptr_t phys);
bool pmm_is_range_poisoned(uintptr_t phys, size_t pages);
size_t pmm_poisoned_page_count(void);

BuddyZone* pmm_get_buddy_zone(void);


void* _pmm_alloc_impl(size_t pages, uint64_t zone_hint);
void* _pmm_alloc_zero_impl(size_t pages, uint64_t zone_hint);

#define _PMM_NARG(...)              _PMM_NARG_I(__VA_ARGS__, 2, 1)
#define _PMM_NARG_I(_1, _2, N, ...) N
#define _PMM_CAT(a, b)              _PMM_CAT_(a, b)
#define _PMM_CAT_(a, b)             a##b

#define _pmm_alloc_1(p)             _pmm_alloc_impl((p), 0ULL)
#define _pmm_alloc_2(p, hint)       _pmm_alloc_impl((p), (uint64_t)(hint))

#define pmm_alloc(...)              _PMM_CAT(_pmm_alloc_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

#define _pmm_alloc_zero_1(p)        _pmm_alloc_zero_impl((p), 0ULL)
#define _pmm_alloc_zero_2(p, hint)  _pmm_alloc_zero_impl((p), (uint64_t)(hint))

#define pmm_alloc_zero(...)         _PMM_CAT(_pmm_alloc_zero_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

void *pmm_alloc_with_keyid(size_t pages, uint16_t keyid);

void pmm_free_with_keyid(void *va, size_t pages, uint16_t keyid);

#endif