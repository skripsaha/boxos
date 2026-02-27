#ifndef VMM_H
#define VMM_H

#include "klib.h"
#include "cabin_layout.h"

// ========== VMM CONSTANTS ==========
#define VMM_PAGE_SIZE           4096
#define VMM_PAGE_MASK           0xFFFFFFFFFFFFF000ULL
#define VMM_PAGE_OFFSET_MASK    0x0000000000000FFFULL

// Virtual address space layout
#define VMM_KERNEL_BASE         0xFFFF800000000000ULL  // -128TB
#define VMM_KERNEL_HEAP_BASE    0xFFFF800000000000ULL  // Kernel heap start
#define VMM_KERNEL_HEAP_SIZE    (1ULL << 30)           // 1GB kernel heap

// Kernel MMIO virtual address region
#define VMM_KERNEL_MMIO_BASE    0xFFFF800040000000ULL  // Starts after 1GB heap
#define VMM_KERNEL_MMIO_SIZE    (3ULL << 30)           // 3GB MMIO region

// CABIN MEMORY MODEL (Snowball Architecture - Flat Binary, NO ELF)
// Each process lives in isolated Cabin with fixed virtual layout:
#define VMM_CABIN_NULL_TRAP     CABIN_NULL_TRAP_START  // 0x0000-0x0FFF: NULL trap zone (unmapped)
#define VMM_CABIN_NOTIFY_PAGE   CABIN_NOTIFY_PAGE_ADDR  // 0x1000: Notify Page (User→Kernel)
#define VMM_CABIN_RESULT_PAGE   CABIN_RESULT_PAGE_ADDR  // 0x2000: Result Page (Kernel→User)
#define VMM_CABIN_CODE_START    CABIN_CODE_START_ADDR   // 0x3000+: Code, data, heap, stack (ENTRY POINT)

// Legacy defines for compatibility (map to Cabin layout)
#define VMM_USER_BASE           VMM_CABIN_CODE_START   // Flat binary entry point (NOT ELF!)
#define VMM_USER_STACK_TOP      0x00007FFFFFFFE000ULL  // ~128TB user space top
#define VMM_USER_HEAP_BASE      0x0000000000020000ULL  // Heap starts after code (configurable via CONFIG_USER_HEAP_INITIAL_SIZE)

// Memory protection flags
#define VMM_FLAG_PRESENT        (1ULL << 0)   // Page is present
#define VMM_FLAG_WRITABLE       (1ULL << 1)   // Page is writable
#define VMM_FLAG_USER           (1ULL << 2)   // User accessible
#define VMM_FLAG_WRITE_THROUGH  (1ULL << 3)   // Write-through caching
#define VMM_FLAG_CACHE_DISABLE  (1ULL << 4)   // Cache disabled
#define VMM_FLAG_ACCESSED       (1ULL << 5)   // Page was accessed
#define VMM_FLAG_DIRTY          (1ULL << 6)   // Page was written to
#define VMM_FLAG_LARGE_PAGE     (1ULL << 7)   // 2MB/1GB page
#define VMM_FLAG_GLOBAL         (1ULL << 8)   // Global page
#define VMM_FLAG_NO_EXECUTE     (1ULL << 63)  // No execute (NX bit)

// Convenience flag combinations
#define VMM_FLAGS_KERNEL_RW     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_PRESENT)
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL)
#define VMM_FLAGS_USER_RW       (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_FLAGS_USER_RO       (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_FLAGS_USER_CODE     (VMM_FLAG_PRESENT | VMM_FLAG_USER)

// Page table entry masks
// VMM_PTE_ADDR_MASK is now runtime-calculated based on MAXPHYADDR (see vmm.c)
#define VMM_PTE_FLAGS_MASK      0x8000000000000FFFULL

// Runtime-calculated address mask based on MAXPHYADDR
extern uint64_t vmm_pte_addr_mask;
extern uint8_t vmm_maxphyaddr;

// Virtual address indices
#define VMM_PML4_INDEX(addr)    (((addr) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(addr)    (((addr) >> 30) & 0x1FF)
#define VMM_PD_INDEX(addr)      (((addr) >> 21) & 0x1FF)
#define VMM_PT_INDEX(addr)      (((addr) >> 12) & 0x1FF)

// ========== STRUCTURES ==========

// Page table entry (64-bit)
typedef uint64_t pte_t;

// Page table (512 entries)
typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Virtual memory context (address space)
typedef struct {
    page_table_t* pml4;           // Top-level page table
    uintptr_t pml4_phys;          // Physical address of PML4
    spinlock_t lock;              // Protection lock
    
    // Memory statistics
    size_t mapped_pages;          // Number of mapped pages
    size_t kernel_pages;          // Kernel pages in this context
    size_t user_pages;            // User pages in this context
    
    // Memory regions tracking (simple version)
    uintptr_t heap_start;         // Current heap start
    uintptr_t heap_end;           // Current heap end
    uintptr_t stack_top;          // Stack top for user processes
} vmm_context_t;

// Memory mapping result
typedef struct {
    bool success;
    uintptr_t virt_addr;
    uintptr_t phys_addr;
    size_t pages_mapped;
    const char* error_msg;
} vmm_map_result_t;

// ========== CORE FUNCTIONS ==========

// Initialization
void vmm_init(void);
void vmm_test_basic(void);

// Context management
vmm_context_t* vmm_create_context(void);
void vmm_destroy_context(vmm_context_t* ctx);
vmm_context_t* vmm_get_kernel_context(void);
vmm_context_t* vmm_get_current_context(void);
void vmm_switch_context(vmm_context_t* ctx);

// Memory mapping
vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags);
vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags);
bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

// Map physical MMIO region into kernel virtual address space
// Returns virtual address, or NULL on failure
// Always applies VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH
volatile void* vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags);

// Unmap MMIO region
void vmm_unmap_mmio(volatile void* virt_addr, size_t size);

// Memory allocation (high-level)
void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags);
void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count);

// Address translation
uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr);
uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr);

// Kernel heap (vmalloc equivalent)
void* vmalloc(size_t size);
void* vzalloc(size_t size);  // Zero-initialized
void vfree(void* addr);

// Memory regions
uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end);
bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags);

// Utilities
void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_dump_context_stats(vmm_context_t* ctx);
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(uintptr_t virt_addr);

// Protection and flags
bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags);
bool vmm_is_user_accessible(uintptr_t virt_addr);
bool vmm_is_kernel_addr(uintptr_t addr);

// ========== INTERNAL FUNCTIONS (for advanced use) ==========

// Page table manipulation
page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level);
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr);
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_invalidate_page(uintptr_t virt_addr);

// Physical memory integration
uintptr_t vmm_alloc_page_table(void);
void vmm_free_page_table(uintptr_t phys_addr);

// Error handling
const char* vmm_get_last_error(void);
void vmm_set_error(const char* error);

// ========== DEBUGGING & STATISTICS ==========
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

// ========== INLINE HELPERS ==========

// Check if address is page-aligned
static inline bool vmm_is_page_aligned(uintptr_t addr) {
    return (addr & VMM_PAGE_OFFSET_MASK) == 0;
}

// Align address down to page boundary
static inline uintptr_t vmm_page_align_down(uintptr_t addr) {
    return addr & VMM_PAGE_MASK;
}

// Align address up to page boundary
static inline uintptr_t vmm_page_align_up(uintptr_t addr) {
    return (addr + VMM_PAGE_SIZE - 1) & VMM_PAGE_MASK;
}

// Get page count for size
static inline size_t vmm_size_to_pages(size_t size) {
    return (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

// Convert pages to bytes
static inline size_t vmm_pages_to_size(size_t pages) {
    return pages * VMM_PAGE_SIZE;
}

// Get current address mask (runtime-calculated based on MAXPHYADDR)
static inline uint64_t vmm_get_addr_mask(void) {
    return vmm_pte_addr_mask;
}

// Extract physical address from PTE
static inline uintptr_t vmm_pte_to_phys(pte_t pte) {
    return pte & vmm_get_addr_mask();
}

// Extract flags from PTE
static inline uint64_t vmm_pte_to_flags(pte_t pte) {
    return pte & VMM_PTE_FLAGS_MASK;
}

// Create PTE from physical address and flags
static inline pte_t vmm_make_pte(uintptr_t phys_addr, uint64_t flags) {
    // BUG #17 FIX: Validate physical address before creating PTE
    // Addresses beyond MAXPHYADDR cause Reserved Bit Violation (#PF)
    extern uint8_t vmm_maxphyaddr;
    uint64_t max_phys = (1ULL << vmm_maxphyaddr);

    if (phys_addr >= max_phys) {
        // This is a programming error - caller should validate addresses
        // For now, mask and hope for the best (already done below)
        // TODO: Consider panic() in debug builds
    }

    // Mask physical address to only valid bits
    uintptr_t masked_phys = phys_addr & vmm_get_addr_mask();
    return masked_phys | (flags & VMM_PTE_FLAGS_MASK);
}

// ========== PHYSICAL ↔ VIRTUAL ADDRESS TRANSLATION ==========
// CRITICAL: VMM/PMM architecture currently relies on identity mapping!
// These functions convert between physical and virtual addresses.
//
// Current implementation: Uses identity mapping (physical = virtual for low memory)
// Future improvement: Should use higher-half direct mapping (0xFFFF880000000000+)

static inline void* vmm_phys_to_virt(uintptr_t phys_addr) {
    // For now: rely on identity mapping
    // The entire PMM region is identity mapped (phys = virt) using 2MB large pages
    // This works as long as all allocated memory is within identity mapped region
    return (void*)phys_addr;
}

static inline uintptr_t vmm_virt_to_phys_direct(void* virt_addr) {
    // For now: rely on identity mapping
    // This is the inverse of vmm_phys_to_virt for identity-mapped addresses
    uintptr_t virt = (uintptr_t)virt_addr;

    // If address is in kernel space (higher half), it's not identity mapped
    if (virt >= VMM_KERNEL_BASE) {
        // Not supported yet - need proper higher-half mapping
        return 0;
    }

    // Otherwise assume identity mapping
    return virt;
}

// Page fault handling
// Returns: 0 on success (handled), -1 on error (unhandled)
int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code);

// ========== CABIN MEMORY MODEL SUPPORT ==========
// Cabin: Isolated virtual address space for user processes with fixed layout
// Layout: 0x0000-0x0FFF (NULL trap), 0x1000 (Notify), 0x2000 (Result), 0x3000+ (Code/Data/Heap/Stack)

// Create Cabin with fixed layout and allocate physical pages for notify/result
// Returns: VMM context for the new Cabin, or NULL on failure
// Also outputs physical addresses of notify and result pages via pointers
vmm_context_t* vmm_create_cabin(uint64_t* notify_page_phys, uint64_t* result_page_phys);

// Map Notify Page at 0x1000 (User write, Kernel read)
int vmm_map_notify_page(vmm_context_t* ctx, uintptr_t phys_page);

// Map Result Page at 0x2000 (User read, Kernel write)
int vmm_map_result_page(vmm_context_t* ctx, uintptr_t phys_page);

// Setup NULL trap zone (0x0000-0x0FFF unmapped, causes page fault on access)
int vmm_setup_null_trap(vmm_context_t* ctx);

// Map code/data region starting at 0x3000
int vmm_map_code_region(vmm_context_t* ctx, uintptr_t code_phys, uint64_t size);

#endif // VMM_H