#ifndef VMM_H
#define VMM_H

#include "klib.h"
#include "error.h"
#include "cabin_layout.h"

#define VMM_PAGE_SIZE           4096
#define VMM_PAGE_MASK           0xFFFFFFFFFFFFF000ULL
#define VMM_PAGE_OFFSET_MASK    0x0000000000000FFFULL

#define VMM_LARGE_PAGE_2M_SIZE   0x200000UL
#define VMM_LARGE_PAGE_2M_MASK   (VMM_LARGE_PAGE_2M_SIZE - 1)
#define VMM_LARGE_PAGE_2M_PAGES  (VMM_LARGE_PAGE_2M_SIZE / VMM_PAGE_SIZE)

#define VMM_KERNEL_BASE         0xFFFF800000000000ULL
#define VMM_KERNEL_HEAP_BASE    0xFFFF800000000000ULL
#define VMM_KERNEL_HEAP_SIZE    (1ULL << 30)

#define VMM_KERNEL_MMIO_BASE    0xFFFF800040000000ULL
#define VMM_KERNEL_MMIO_SIZE    (3ULL << 30)

#define PULL_MAP_BASE           0xFFFF880000000000ULL
#define PULL_MAP_PML4_INDEX     272
#define PULL_MAP_SPAN           (512ULL << 30)

#define VMM_CABIN_NULL_TRAP     CABIN_NULL_TRAP_START
#define VMM_CABIN_INFO          CABIN_INFO_ADDR
#define VMM_CABIN_POCKET_RING   CABIN_POCKET_RING_ADDR
#define VMM_CABIN_RESULT_RING   CABIN_RESULT_RING_ADDR
#define VMM_CABIN_TOUCH_RING    CABIN_TOUCH_RING_ADDR
#define VMM_CABIN_CODE_START    CABIN_CODE_START_ADDR

#define VMM_USER_BASE           VMM_CABIN_CODE_START
#define VMM_USER_STACK_TOP      0x00007FFFFFFFE000ULL
#define VMM_USER_HEAP_BASE      CABIN_HEAP_BASE

#define VMM_FLAG_PRESENT        (1ULL << 0)
#define VMM_FLAG_WRITABLE       (1ULL << 1)
#define VMM_FLAG_USER           (1ULL << 2)
#define VMM_FLAG_WRITE_THROUGH  (1ULL << 3)
#define VMM_FLAG_CACHE_DISABLE  (1ULL << 4)
#define VMM_FLAG_ACCESSED       (1ULL << 5)
#define VMM_FLAG_DIRTY          (1ULL << 6)
#define VMM_FLAG_LARGE_PAGE     (1ULL << 7)
#define VMM_FLAG_PAT_BIT        (1ULL << 7)
#define VMM_FLAG_GLOBAL         (1ULL << 8)
#define VMM_FLAG_NO_EXECUTE     (1ULL << 63)

#define VMM_FLAGS_KERNEL_RW     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_PRESENT)
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL)
#define VMM_FLAGS_USER_RW       (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_FLAGS_USER_RO       (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_FLAGS_USER_CODE     (VMM_FLAG_PRESENT | VMM_FLAG_USER)

#define VMM_PTE_FLAGS_MASK      0x8000000000000FFFULL

extern uint64_t vmm_pte_addr_mask;
extern uint64_t vmm_pte_addr_mask_with_keyid;
extern uint8_t vmm_maxphyaddr;

extern uint64_t vmm_pte_flags_mask;
void vmm_note_no_execute(bool usable);

#define VMM_PML5_INDEX(addr)    (((addr) >> 48) & 0x1FF)
#define VMM_PML4_INDEX(addr)    (((addr) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(addr)    (((addr) >> 30) & 0x1FF)
#define VMM_PD_INDEX(addr)      (((addr) >> 21) & 0x1FF)
#define VMM_PT_INDEX(addr)      (((addr) >> 12) & 0x1FF)

typedef uint64_t pte_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

extern int  g_vmm_paging_levels;
extern bool g_vmm_la57_active;

typedef enum {
    VMM_LAM_NONE = 0,
    VMM_LAM_U48  = 1,
    VMM_LAM_U57  = 2,
} vmm_lam_mode_t;

typedef struct {
    page_table_t* pml4;
    uintptr_t pml4_phys;
    uint16_t pcid;
    uint8_t  lam_mode;
    uint8_t  reserved_a;
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

bool vmm_register_shared_phys(uint64_t phys);
bool vmm_is_shared_phys(uint64_t phys);

vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags);
vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags);
bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

vmm_map_result_t vmm_map_page_with_keyid(vmm_context_t* ctx, uintptr_t virt_addr,
                                         uintptr_t phys_addr_with_keyid, uint64_t flags);

bool vmm_map_huge_2m_with_keyid(vmm_context_t* ctx, uintptr_t virt_addr,
                                 uintptr_t phys_addr_with_keyid, uint64_t flags);

volatile void* vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags);
void vmm_unmap_mmio(volatile void* virt_addr, size_t size);

volatile void* vmm_map_framebuffer(uintptr_t phys_addr, size_t size);

void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags);
void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count);

uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr);
uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr);

int vmm_ensure_user_page(vmm_context_t* ctx, uintptr_t user_vaddr, bool writable);

void* vmalloc(size_t size);
void* vzalloc(size_t size);
void vfree(void* addr);

uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end);
bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags);

void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_dump_context_stats(vmm_context_t* ctx);
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(uintptr_t virt_addr);

void vmm_shootdown_page(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_shootdown_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

void vmm_tlb_shootdown_handler(void);

void vmm_tlb_shootdown_poll(void);

bool vmm_pcid_active(void);
uint64_t vmm_build_cr3(vmm_context_t* ctx);
uint64_t vmm_build_cr3_noflush(vmm_context_t* ctx);

bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags);
bool vmm_is_user_accessible(uintptr_t virt_addr);
bool vmm_is_kernel_addr(uintptr_t addr);

page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level);
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr);
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr);

pte_t* vmm_get_leaf_pte(vmm_context_t* ctx, uintptr_t virt_addr, uint8_t* out_level);

extern uint64_t g_ia32_pat_value;

uint8_t      vmm_pte_pat_index(uint64_t pte_val, bool is_huge_leaf);
uint8_t      vmm_pat_type_at(uint8_t pat_idx);
const char  *vmm_pte_cache_type_str(uint64_t pte_val, bool is_huge_leaf);
uint64_t     vmm_get_pat_msr_value(void);

uint64_t     vmm_wc_pte_flags(void);

bool         vmm_verify_pat_msr(void);
void         vmm_dump_mtrr_layout(void);

bool         vmm_verify_pte_metadata_bits_52_58(void);

#define VMM_PTE_PKEY_SHIFT    59
#define VMM_PTE_PKEY_BITS     4
#define VMM_PTE_PKEY_MAX      ((1u << VMM_PTE_PKEY_BITS) - 1u)
#define VMM_PTE_PKEY_MASK     (((uint64_t)VMM_PTE_PKEY_MAX) << VMM_PTE_PKEY_SHIFT)

static inline uint64_t vmm_pte_encode_pkey(uint8_t pkey) {
    return (((uint64_t)pkey) & VMM_PTE_PKEY_MAX) << VMM_PTE_PKEY_SHIFT;
}
static inline uint8_t vmm_pte_pkey(uint64_t pte_val) {
    return (uint8_t)((pte_val >> VMM_PTE_PKEY_SHIFT) & VMM_PTE_PKEY_MAX);
}
static inline bool vmm_pte_pkey_cet_conflict(uint64_t flags) {
    bool any_pkey = (flags & VMM_PTE_PKEY_MASK & ~(1ULL << 60)) != 0ULL;
    bool cet_ss   = (flags & (1ULL << 60)) != 0ULL;
    return any_pkey && cet_ss;
}
static inline uint8_t vmm_pte_pkey_effective(uint64_t pte_val) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    uint64_t mask = (cr4 & (1ULL << 23))
                    ? (VMM_PTE_PKEY_MASK & ~(1ULL << 60))
                    : VMM_PTE_PKEY_MASK;
    return (uint8_t)((pte_val & mask) >> VMM_PTE_PKEY_SHIFT);
}

void vmm_pku_init(void);

void vmm_pku_ap_init(void);

uint32_t vmm_read_pkru(void);
void     vmm_write_pkru(uint32_t value);

#define VMM_CR3_LAM_U57    (1ULL << 61)
#define VMM_CR3_LAM_U48    (1ULL << 62)
#define VMM_CR3_LAM_MASK   (VMM_CR3_LAM_U48 | VMM_CR3_LAM_U57)
#define VMM_CR4_LAM_SUP    (1ULL << 28)

static inline uint64_t vmm_lam_u48_tag_mask(void) { return 0x7FULL << 56; }
static inline uint64_t vmm_lam_u57_tag_mask(void) { return 0x3FULL << 57; }

static inline uint64_t vmm_user_ptr_set_tag_u48(uint64_t ptr, uint8_t tag) {
    return (ptr & ~vmm_lam_u48_tag_mask()) | ((((uint64_t)tag) & 0x7F) << 56);
}
static inline uint8_t  vmm_user_ptr_get_tag_u48(uint64_t ptr) {
    return (uint8_t)((ptr >> 56) & 0x7F);
}
static inline uint64_t vmm_user_ptr_set_tag_u57(uint64_t ptr, uint8_t tag) {
    return (ptr & ~vmm_lam_u57_tag_mask()) | ((((uint64_t)tag) & 0x3F) << 57);
}
static inline uint8_t  vmm_user_ptr_get_tag_u57(uint64_t ptr) {
    return (uint8_t)((ptr >> 57) & 0x3F);
}

void vmm_lam_probe(void);
void vmm_lam_ap_probe(void);

error_t vmm_set_user_lam(vmm_context_t *ctx, vmm_lam_mode_t mode);

#define VMM_MSR_IA32_TME_CAPABILITY  0x981U
#define VMM_MSR_IA32_TME_ACTIVATE    0x982U

#define VMM_TME_ACT_LOCK             (1ULL << 0)
#define VMM_TME_ACT_TME_EN           (1ULL << 1)
#define VMM_TME_ACT_TME_MK_EN        (1ULL << 36)
#define VMM_TME_ACT_KEYID_BITS_MASK  (0xFULL << 32)
#define VMM_TME_ACT_KEYID_BITS_SHIFT 32

void vmm_tme_probe(void);
void vmm_tme_ap_probe(void);

#define VMM_CR4_CET_BIT          (1ULL << 23)
#define VMM_MSR_IA32_U_CET       0x6A0U
#define VMM_MSR_IA32_S_CET       0x6A2U
#define VMM_MSR_IA32_PL0_SSP     0x6A4U
#define VMM_MSR_IA32_PL3_SSP     0x6A7U

#define VMM_PTE_CET_SS_SUPV      (1ULL << 60)
#define VMM_PTE_CET_SS_USER      (1ULL << 61)

#define VMM_XCR0_CET_S_BIT       (1ULL << 11)
#define VMM_XCR0_CET_U_BIT       (1ULL << 12)

void vmm_cet_probe(void);
void vmm_cet_ap_probe(void);

uint16_t vmm_get_cet_cp_tag(void);

static inline uint64_t vmm_pte_with_cet_supv_ss(uint64_t pte) {
    return pte | VMM_PTE_CET_SS_SUPV;
}
static inline bool vmm_pte_is_supv_ss(uint64_t pte) {
    return (pte & VMM_PTE_CET_SS_SUPV) != 0;
}

static inline uint64_t vmm_phys_with_keyid(uint64_t phys, uint8_t keyid,
                                            uint8_t num_keyid_bits,
                                            uint8_t reduced_maxphyaddr) {
    if (num_keyid_bits == 0 ||
        num_keyid_bits >= 64 ||
        reduced_maxphyaddr >= 64 ||
        (uint32_t)num_keyid_bits + (uint32_t)reduced_maxphyaddr > 64) {
        return phys;
    }
    uint64_t mask = ((1ULL << num_keyid_bits) - 1ULL) << reduced_maxphyaddr;
    return (phys & ~mask) | (((uint64_t)keyid << reduced_maxphyaddr) & mask);
}

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
    extern uint8_t vmm_maxphyaddr;
    uint64_t max_phys = (1ULL << vmm_maxphyaddr);

    if (phys_addr >= max_phys) {
        debug_printf("[VMM] ERROR: phys_addr 0x%lx exceeds MAXPHYADDR (%u bits, max 0x%lx)\n",
                     phys_addr, vmm_maxphyaddr, max_phys);
    }

    uintptr_t masked_phys = phys_addr & vmm_get_addr_mask();
    return masked_phys | (flags & vmm_pte_flags_mask);
}

static inline pte_t vmm_make_pte_with_keyid(uintptr_t phys_with_keyid, uint64_t flags) {
    uintptr_t masked = phys_with_keyid & vmm_pte_addr_mask_with_keyid;
    return masked | (flags & vmm_pte_flags_mask);
}

void* vmm_phys_to_virt(uintptr_t phys_addr);

static inline uintptr_t vmm_virt_to_phys_direct(void* virt_addr) {
    uintptr_t virt = (uintptr_t)virt_addr;
    if (virt >= PULL_MAP_BASE) {
        return virt - PULL_MAP_BASE;
    }
    if (virt >= 0xFFFFFFFF80000000ULL) {
        return virt - 0xFFFFFFFF80000000ULL;
    }
    if (virt >= VMM_KERNEL_BASE) {
        return 0;
    }
    return virt;
}

int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code);

vmm_context_t* vmm_create_cabin(uint64_t* cabin_info_phys,
                                uint64_t* pocket_ring_phys,
                                uint64_t* result_ring_phys,
                                uint64_t* touch_ring_phys);
int vmm_map_cabin_info(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_pocket_ring(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_result_ring(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_touch_ring(vmm_context_t* ctx, uintptr_t phys_page);

void* vmm_translate_user_addr(vmm_context_t* ctx, uintptr_t user_vaddr, size_t size);

void *vmm_user_buf_in(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size);

error_t vmm_user_buf_in_into(vmm_context_t *ctx, uintptr_t user_vaddr,
                              size_t size, void *kbuf);

void   *vmm_user_buf_alloc_out(size_t size);
error_t vmm_user_buf_commit_out(vmm_context_t *ctx, uintptr_t user_vaddr,
                                 const void *kbuf, size_t size);
void    vmm_user_buf_free(void *kbuf);
int vmm_setup_null_trap(vmm_context_t* ctx);
int vmm_map_code_region(vmm_context_t* ctx, uintptr_t code_phys, uint64_t size,
                        uintptr_t *out_entry);

void pmm_activate_pull_map(void);

void vmm_pat_init(void);

bool vmm_map_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr,
                     uintptr_t phys_addr, uint64_t flags);

bool vmm_unmap_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr);

uintptr_t vmm_virt_to_phys_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr);

void vmm_shootdown_all_cores_full(void);

#endif