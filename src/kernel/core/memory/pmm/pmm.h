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

// Инициализация PMM
void pmm_init(void);

// Основные функции
void* pmm_alloc(size_t pages);
void* pmm_alloc_zero(size_t pages);
void pmm_free(void* addr, size_t pages);

// Утилиты
size_t pmm_total_pages(void);
size_t pmm_free_pages(void);
size_t pmm_used_pages(void);
uint64_t pmm_get_total_memory(void);
void pmm_dump_stats(void);

// Отладочные функции
void pmm_print_memory_map(void);
bool pmm_check_integrity(void);

// MAXPHYADDR limit (optional override, auto-detected via CPUID if not set)
void pmm_set_maxphyaddr(uint8_t maxphyaddr);
uint8_t pmm_get_maxphyaddr(void);

// Memory boundaries
uint64_t pmm_get_mem_end(void);

/**
 * pmm_is_usable_ram - Check if physical range overlaps USABLE RAM
 * @phys_addr: Start of physical address range
 * @size: Size of range in bytes
 *
 * Checks E820 memory map to determine if [phys_addr, phys_addr + size)
 * overlaps with any E820_USABLE region. Used by VMM to prevent mapping
 * managed RAM as MMIO.
 *
 * Returns: true if range overlaps USABLE RAM, false otherwise
 */
bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size);

// Comprehensive test suite (defined in pmm_test.c)
void pmm_run_tests(void);

#endif // PMM_H