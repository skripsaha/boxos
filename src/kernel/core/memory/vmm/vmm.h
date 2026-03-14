#ifndef VMM_H
#define VMM_H

#include "klib.h"
#include "cabin_layout.h"

#define VMM_PAGE_SIZE           4096
#define VMM_PAGE_MASK           0xFFFFFFFFFFFFF000ULL
#define VMM_PAGE_OFFSET_MASK    0x0000000000000FFFULL

#define VMM_KERNEL_BASE         0xFFFF800000000000ULL  // -128TB
#define VMM_KERNEL_HEAP_BASE    0xFFFF800000000000ULL
#define VMM_KERNEL_HEAP_SIZE    (1ULL << 30)           // 1GB kernel heap

#define VMM_KERNEL_MMIO_BASE    0xFFFF800040000000ULL  // starts after 1GB heap
#define VMM_KERNEL_MMIO_SIZE    (3ULL << 30)           // 3GB MMIO region

// Pull Map: all physical RAM mapped at fixed offset in kernel higher-half.
// Replaces identity mapping. Immutable after boot, uses Global pages.
#define PULL_MAP_BASE           0xFFFF880000000000ULL
#define PULL_MAP_PML4_INDEX     272   // (PULL_MAP_BASE >> 39) & 0x1FF

// CABIN MEMORY MODEL (Snowball Architecture - Flat Binary, NO ELF)
// Each process lives in isolated Cabin with fixed virtual layout:
#define VMM_CABIN_NULL_TRAP     CABIN_NULL_TRAP_START    // 0x0000-0x0FFF: NULL trap zone (unmapped)
#define VMM_CABIN_INFO          CABIN_INFO_ADDR          // 0x1000: CabinInfo (4KB, read-only)
#define VMM_CABIN_POCKET_RING   CABIN_POCKET_RING_ADDR   // 0x2000: PocketRing (4KB, user RW)
#define VMM_CABIN_RESULT_RING   CABIN_RESULT_RING_ADDR   // 0x3000: ResultRing (36KB, user RW)
#define VMM_CABIN_CODE_START    CABIN_CODE_START_ADDR    // 0xC000+: Code, data, heap, stack (ENTRY POINT)

#define VMM_USER_BASE           VMM_CABIN_CODE_START   // flat binary entry point (NOT ELF!)
#define VMM_USER_STACK_TOP      0x00007FFFFFFFE000ULL  // ~128TB user space top
#define VMM_USER_HEAP_BASE      CABIN_HEAP_BASE        // 256MB: fixed heap base (above identity map)

#define VMM_FLAG_PRESENT        (1ULL << 0)
#define VMM_FLAG_WRITABLE       (1ULL << 1)
#define VMM_FLAG_USER           (1ULL << 2)
#define VMM_FLAG_WRITE_THROUGH  (1ULL << 3)
#define VMM_FLAG_CACHE_DISABLE  (1ULL << 4)
#define VMM_FLAG_ACCESSED       (1ULL << 5)
#define VMM_FLAG_DIRTY          (1ULL << 6)
#define VMM_FLAG_LARGE_PAGE     (1ULL << 7)  // 2MB/1GB page
#define VMM_FLAG_GLOBAL         (1ULL << 8)
#define VMM_FLAG_NO_EXECUTE     (1ULL << 63) // NX bit

#define VMM_FLAGS_KERNEL_RW     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_PRESENT)
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL)
#define VMM_FLAGS_USER_RW       (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_FLAGS_USER_RO       (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_FLAGS_USER_CODE     (VMM_FLAG_PRESENT | VMM_FLAG_USER)

// vmm_pte_addr_mask is runtime-calculated based on MAXPHYADDR (see vmm.c)
#define VMM_PTE_FLAGS_MASK      0x8000000000000FFFULL

extern uint64_t vmm_pte_addr_mask;
extern uint8_t vmm_maxphyaddr;

#define VMM_PML4_INDEX(addr)    (((addr) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(addr)    (((addr) >> 30) & 0x1FF)
#define VMM_PD_INDEX(addr)      (((addr) >> 21) & 0x1FF)
#define VMM_PT_INDEX(addr)      (((addr) >> 12) & 0x1FF)

typedef uint64_t pte_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

typedef struct {
    page_table_t* pml4;
    uintptr_t pml4_phys;
    uint16_t pcid;              // Process Context Identifier (0=kernel, 1-4095=user)
    spinlock_t lock;
    size_t mapped_pages;
    size_t kernel_pages;
    size_t user_pages;
    uintptr_t heap_start;
    uintptr_t heap_end;
    uintptr_t stack_top;
} vmm_context_t;

typedef struct {
    bool success;
    uintptr_t virt_addr;
    uintptr_t phys_addr;
    size_t pages_mapped;
    const char* error_msg;
} vmm_map_result_t;

void vmm_init(void);
void vmm_test_basic(void);

vmm_context_t* vmm_create_context(void);
void vmm_destroy_context(vmm_context_t* ctx);
vmm_context_t* vmm_get_kernel_context(void);
vmm_context_t* vmm_get_current_context(void);
void vmm_switch_context(vmm_context_t* ctx);

vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags);
vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags);
bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

// Always applies VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH
// Returns virtual address, or NULL on failure
volatile void* vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags);
void vmm_unmap_mmio(volatile void* virt_addr, size_t size);

void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags);
void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count);

uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr);
uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr);

void* vmalloc(size_t size);
void* vzalloc(size_t size);
void vfree(void* addr);

uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end);
bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags);

void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_dump_context_stats(vmm_context_t* ctx);
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(uintptr_t virt_addr);

// PCID (Process Context Identifiers) — zero-flush context switches
bool vmm_pcid_active(void);
uint64_t vmm_build_cr3(vmm_context_t* ctx);
uint64_t vmm_build_cr3_noflush(vmm_context_t* ctx);

bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags);
bool vmm_is_user_accessible(uintptr_t virt_addr);
bool vmm_is_kernel_addr(uintptr_t addr);

page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level);
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr);
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_invalidate_page(uintptr_t virt_addr);

uintptr_t vmm_alloc_page_table(void);
void vmm_free_page_table(uintptr_t phys_addr);

const char* vmm_get_last_error(void);
void vmm_set_error(const char* error);

typedef struct {
    size_t total_contexts;
    size_t total_mapped_pages;
    size_t kernel_mapped_pages;
    size_t user_mapped_pages;
    size_t page_tables_allocated;
    size_t page_faults_handled;
    size_t tlb_flushes;
} vmm_stats_t;

void vmm_get_global_stats(vmm_stats_t* stats);
void vmm_print_stats(void);

static inline bool vmm_is_page_aligned(uintptr_t addr) {
    return (addr & VMM_PAGE_OFFSET_MASK) == 0;
}

static inline uintptr_t vmm_page_align_down(uintptr_t addr) {
    return addr & VMM_PAGE_MASK;
}

static inline uintptr_t vmm_page_align_up(uintptr_t addr) {
    return (addr + VMM_PAGE_SIZE - 1) & VMM_PAGE_MASK;
}

static inline size_t vmm_size_to_pages(size_t size) {
    return (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

static inline size_t vmm_pages_to_size(size_t pages) {
    return pages * VMM_PAGE_SIZE;
}

static inline uint64_t vmm_get_addr_mask(void) {
    return vmm_pte_addr_mask;
}

static inline uintptr_t vmm_pte_to_phys(pte_t pte) {
    return pte & vmm_get_addr_mask();
}

static inline uint64_t vmm_pte_to_flags(pte_t pte) {
    return pte & VMM_PTE_FLAGS_MASK;
}

static inline pte_t vmm_make_pte(uintptr_t phys_addr, uint64_t flags) {
    // addresses beyond MAXPHYADDR set reserved bits and cause #PF
    extern uint8_t vmm_maxphyaddr;
    uint64_t max_phys = (1ULL << vmm_maxphyaddr);

    if (phys_addr >= max_phys) {
        debug_printf("[VMM] ERROR: phys_addr 0x%lx exceeds MAXPHYADDR (%u bits, max 0x%lx)\n",
                     phys_addr, vmm_maxphyaddr, max_phys);
    }

    uintptr_t masked_phys = phys_addr & vmm_get_addr_mask();
    return masked_phys | (flags & VMM_PTE_FLAGS_MASK);
}

// Pull Map: converts physical address to kernel-accessible virtual address.
// Before Pull Map is activated (during early boot), returns identity mapping.
// After activation, returns phys + PULL_MAP_BASE.
void* vmm_phys_to_virt(uintptr_t phys_addr);

static inline uintptr_t vmm_virt_to_phys_direct(void* virt_addr) {
    uintptr_t virt = (uintptr_t)virt_addr;
    if (virt >= PULL_MAP_BASE) {
        return virt - PULL_MAP_BASE;
    }
    if (virt >= VMM_KERNEL_BASE) {
        return 0;
    }
    return virt;
}

// Returns 0 on success (handled), -1 on error (unhandled)
int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code);

// Cabin: isolated virtual address space for user processes.
// Layout: 0x0000 (NULL trap), 0x1000 (CabinInfo RO), 0x2000 (PocketRing RW),
//         0x3000 (ResultRing RW), 0xC000+ (Code/Data/Heap/Stack)
vmm_context_t* vmm_create_cabin(uint64_t* cabin_info_phys, uint64_t* pocket_ring_phys, uint64_t* result_ring_phys);
int vmm_map_cabin_info(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_pocket_ring(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_result_ring(vmm_context_t* ctx, uintptr_t phys_page);

// Translate a user virtual address in a process's page table to a kernel-accessible pointer.
// Walks the process's page tables, resolves the physical address, and returns it as a
// kernel pointer via Pull Map. Returns NULL if the address is not mapped.
void* vmm_translate_user_addr(vmm_context_t* ctx, uintptr_t user_vaddr, size_t size);
int vmm_setup_null_trap(vmm_context_t* ctx);
int vmm_map_code_region(vmm_context_t* ctx, uintptr_t code_phys, uint64_t size);

// Called by vmm_init() after Pull Map is live to rebase PMM bitmap pointer
void pmm_activate_pull_map(void);

#endif // VMM_H
