#ifndef PMM_H
#define PMM_H

#include "klib.h"

#define PMM_PAGE_SIZE       4096
#define PMM_BITMAP_ALIGN    8
#define PMM_MAX_MEMORY      (128ULL * 1024 * 1024 * 1024) // 128GB

typedef enum {
    PMM_FRAME_FREE = 0,
    PMM_FRAME_USED,
    PMM_FRAME_RESERVED,
    PMM_FRAME_KERNEL,
    PMM_FRAME_BAD
} pmm_frame_state_t;

void pmm_init(void);

void* pmm_alloc(size_t pages);
void* pmm_alloc_zero(size_t pages);
void pmm_free(void* addr, size_t pages);

size_t pmm_total_pages(void);
size_t pmm_free_pages(void);
size_t pmm_used_pages(void);
uint64_t pmm_get_total_memory(void);
void pmm_dump_stats(void);

void pmm_print_memory_map(void);
bool pmm_check_integrity(void);

// override auto-detected MAXPHYADDR for VMM customization
void pmm_set_maxphyaddr(uint8_t maxphyaddr);
uint8_t pmm_get_maxphyaddr(void);

uint64_t pmm_get_mem_end(void);

// returns true if [phys_addr, phys_addr+size) overlaps any E820_USABLE region;
// used by VMM to refuse mapping managed RAM as MMIO
bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size);

void pmm_run_tests(void);

#endif // PMM_H
