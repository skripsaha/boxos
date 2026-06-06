#ifndef VMM_H
#define VMM_H

#include "klib.h"
#include "error.h"
#include "cabin_layout.h"

#define VMM_PAGE_SIZE           4096
#define VMM_PAGE_MASK           0xFFFFFFFFFFFFF000ULL
#define VMM_PAGE_OFFSET_MASK    0x0000000000000FFFULL

/* 2 MiB huge page — single PDE leaf with VMM_FLAG_LARGE_PAGE. Used by
 * Bay (cross-cabin shared memory), the user-heap implicit-huge path
 * (SYSTEM_OP_HEAP_PREFAULT), and future drivers requiring 2 MiB-aligned
 * DMA. Sizes/masks centralized here so callers stop sprinkling
 * 0x200000 / 0x1FFFFF / 512 across the codebase.
 *
 * VMM_LARGE_PAGE_2M_PAGES is the number of 4 KiB PMM pages backing one
 * 2 MiB chunk — the unit pmm_alloc() takes for order-9 allocations and
 * the value used in mapped-page accounting. */
#define VMM_LARGE_PAGE_2M_SIZE   0x200000UL
#define VMM_LARGE_PAGE_2M_MASK   (VMM_LARGE_PAGE_2M_SIZE - 1)
#define VMM_LARGE_PAGE_2M_PAGES  (VMM_LARGE_PAGE_2M_SIZE / VMM_PAGE_SIZE)

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
#define VMM_CABIN_POCKET_RING   CABIN_POCKET_RING_ADDR   // 0x2000: PocketRing (4KB header, user RW)
#define VMM_CABIN_RESULT_RING   CABIN_RESULT_RING_ADDR   // 0x3000: ResultRing (4KB header, user RW)
#define VMM_CABIN_TOUCH_RING    CABIN_TOUCH_RING_ADDR    // 0x5000: TouchRing (4KB header, user RW)
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
#define VMM_FLAG_LARGE_PAGE     (1ULL << 7)  // 2MB/1GB page (PS bit in PDE)
/*
 * VMM_FLAG_PAT_BIT — bit 7 in a *leaf 4 KB PTE* is the PAT selector bit.
 * Combined with PCD and PWT it selects the PAT entry (index 0–7) from the
 * IA32_PAT MSR.  vmm_pat_init() programs entry 6 to Write Combining (WC):
 *   PAT_bit=1, PCD=1, PWT=0  →  PAT index = (1<<2)|(1<<1)|0 = 6 = WC
 * Use this flag together with VMM_FLAG_CACHE_DISABLE for framebuffer pages.
 * IMPORTANT: never set this flag on intermediate PDE/PDPT entries — bit 7
 * there is PS (page size) and enables large pages, not PAT selection.
 */
#define VMM_FLAG_PAT_BIT        (1ULL << 7)  // PAT selector in leaf 4KB PTEs
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

/* Shared-physical registry. Pages registered here are mapped into many
 * Cabins (cpu_caps page, ClockBoard, future vDSO-style pages) but live
 * for the whole kernel session. vmm_destroy_context skips them — without
 * this, the second process to be torn down hits a buddy double-free
 * panic on the same physical. Call once at boot, after the page is
 * allocated; safe to register before any Cabin is created.
 *
 * Returns true on success, false if the registry is full or phys is 0.
 * Idempotent: registering the same phys twice is a silent no-op.        */
bool vmm_register_shared_phys(uint64_t phys);
bool vmm_is_shared_phys(uint64_t phys);

vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags);
vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags);
bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

// Always applies VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH → UC mapping.
// Returns virtual address, or NULL on failure.
volatile void* vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags);
void vmm_unmap_mmio(volatile void* virt_addr, size_t size);

/*
 * vmm_map_framebuffer — map a linear framebuffer with Write Combining (WC).
 *
 * Unlike vmm_map_mmio (which forces UC = Uncacheable), this function uses
 * WC caching: the CPU coalesces writes into cache-line bursts before pushing
 * them to the bus, making sequential blits 10–50× faster.
 *
 * Requires vmm_pat_init() to have been called during vmm_init() so that
 * IA32_PAT entry 6 is programmed to WC (type 1).
 *
 * PTE flags: VMM_FLAG_CACHE_DISABLE | VMM_FLAG_PAT_BIT (no WRITE_THROUGH)
 *   → PCD=1, PAT=1, PWT=0 → PAT index 6 = WC.
 *
 * Virtual address is allocated from the kernel MMIO bump region, same pool
 * as vmm_map_mmio, so the two functions never overlap.
 */
volatile void* vmm_map_framebuffer(uintptr_t phys_addr, size_t size);

void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags);
void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count);

uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr);
uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr);

/* Ensure that the page covering `user_vaddr` is mapped user RW (and NX) in
 * `ctx`. If already mapped, returns 0 immediately. If unmapped, allocates a
 * zeroed phys page and maps it. Used by the kernel-side ResultRing producer
 * (see kring.c) to map slot pages on demand without triggering a kernel
 * page fault. Returns 0 on success, -1 on failure. */
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

// TLB Shootdown: cross-core TLB invalidation via IPI.
// Called after vmm_unmap/vmm_map/vmm_protect to flush stale entries on
// remote cores that share the same address space (vmm_context_t).
// Single-core: local invlpg only. Multi-core: IPI to affected cores.
void vmm_shootdown_page(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_shootdown_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

// Called from IPI_SHOOTDOWN_VECTOR handler in idt.c.
void vmm_tlb_shootdown_handler(void);

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

/*
 * Walk to whichever PT/PD/PDPT entry is the leaf for `virt_addr`.
 * out_level is set to:
 *   1 → 4 KiB PT entry
 *   2 → 2 MiB PD leaf (VMM_FLAG_LARGE_PAGE set on returned entry)
 *   3 → 1 GiB PDPT leaf
 * Returns NULL if no mapping covers `virt_addr` at any level.
 * Phase 2C (memtag enforcement) uses this to manipulate PRESENT bit on
 * mappings independent of page size.
 */
pte_t* vmm_get_leaf_pte(vmm_context_t* ctx, uintptr_t virt_addr, uint8_t* out_level);

/* Phase 2E — PAT MSR + cache-type decoding.
 *
 * vmm_pat_init() programs IA32_PAT (MSR 0x277) at boot. The 64-bit MSR
 * value is snapshot in g_ia32_pat_value so:
 *   1. Runtime helpers can decode any leaf PTE's effective cache type.
 *   2. The MemTag AP-side consistency probe (MemTagVerifyPatMsr) can
 *      compare each AP's RDMSR against the BSP value — Intel SDM Vol 3A
 *      §11.12.4 mandates an identical PAT across coherent CPUs.
 *
 * vmm_pte_pat_index extracts the 3-bit PAT selector (PWT|PCD|PAT) from
 * a leaf PTE, taking into account the bit shift difference between 4 KiB
 * (PAT@bit 7) and 2 MiB/1 GiB (PAT@bit 12) leaves per Intel SDM §11.12.3.
 */
extern uint64_t g_ia32_pat_value;

uint8_t      vmm_pte_pat_index(uint64_t pte_val, bool is_huge_leaf);
uint8_t      vmm_pat_type_at(uint8_t pat_idx);
const char  *vmm_pte_cache_type_str(uint64_t pte_val, bool is_huge_leaf);
uint64_t     vmm_get_pat_msr_value(void);

/* Phase 2E — CPU consistency probes (renamed in audit pass — these are
 * PURE CPU MSR probes with no MemTag-specific state). Verify the PAT
 * MSR is identical across coherent CPUs per Intel SDM Vol 3A §11.12.4;
 * dump firmware-programmed MTRR layout per Intel SDM §11.11/§11.12.5. */
bool         vmm_verify_pat_msr(void);
void         vmm_dump_mtrr_layout(void);

/* Phase 2H — Protection Keys (Intel SDM Vol 3A §4.6.2 / §4.6.3).
 * Bits 62:59 of every leaf PTE form a 4-bit PKEY. The same field is
 * checked against IA32_PKRU for user pages (U=1) and against IA32_PKRS
 * for supervisor pages (U=0) — orthogonal to NX (bit 63), Phase 2D's
 * region id (bits 58:52), and CET (bit 60 — only when CR4.CET=1; we
 * verify gating in vmm_pku_init). */
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

/* BSP-side PKU/PKS bring-up: sets CR4.PKE (bit 22) when has_pku, CR4.PKS
 * (bit 24) when has_pks, and XSETBV XCR0.PKRU (bit 9) when has_xsave +
 * has_pku. Backward-compatible: default PKRU=0 means every key access
 * is allowed, so enabling CR4.PKE has no behavior change until a thread
 * explicitly writes PKRU. */
void vmm_pku_init(void);

/* Per-AP PKU/PKS bring-up. Same CR4 + XCR0 toggles on the calling AP.
 * Called from per_core_init_ap after cpu_intersect_features_ap so a
 * hybrid SKU's PKU-less AP isn't fed a CR4.PKE write that would still
 * be safe but mismatched with the package-wide capability snapshot. */
void vmm_pku_ap_init(void);

/* Read / write IA32_PKRU MSR (0x6E0). Returns 0 on systems without PKU
 * (caller may treat as "all keys allowed"). */
uint32_t vmm_read_pkru(void);
void     vmm_write_pkru(uint32_t value);

/* Phase 2I — Linear Address Masking (LAM) infrastructure.
 *
 * Intel SDM Vol 3A §5.6. Per-CR3 (per-process) opt-in:
 *   CR3.LAM_U48 (bit 62) — bits 62:48 of user VA become "ignored"
 *                          (7 tag bits available in bits 62:56)
 *   CR3.LAM_U57 (bit 61) — bits 62:57 ignored (6 tag bits, for the
 *                          5-level paging variant)
 *   CR4.LAM_SUP (bit 28) — supervisor-side LAM, kernel only
 *
 * Substrate for HWASAN-style tagged pointers: userspace stores a tag
 * in the upper bits, the hardware silently masks them at translation.
 * The OS must (a) preserve LAM bits across context switches via
 * vmm_build_cr3, (b) NEVER treat LAM bits as part of the PML4 phys
 * (use vmm_pte_addr_mask which strips them naturally), (c) gate any
 * future per-process opt-in API on has_lam.
 *
 * Phase 2I lays the bit-layer foundation + BSP/AP probe. Per-process
 * LAM opt-in (CR3 bit OR-in) is the follow-up; until then LAM is
 * dormant and CR3 still carries LAM bits = 0 = no masking.
 */
#define VMM_CR3_LAM_U57    (1ULL << 61)
#define VMM_CR3_LAM_U48    (1ULL << 62)
#define VMM_CR3_LAM_MASK   (VMM_CR3_LAM_U48 | VMM_CR3_LAM_U57)
#define VMM_CR4_LAM_SUP    (1ULL << 28)

/* LAM tag-bit field accessors. LAM_U48 → bits 62:56 (7 bits), LAM_U57
 * → bits 62:57 (6 bits). Bit 63 stays sign-extension / canonical.
 * Pure functions; safe to call regardless of has_lam (the substrate
 * is just bit manipulation). */
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

/* BSP-side LAM probe. No CR3 writes — Phase 2I is observe-only; the
 * per-process opt-in path will OR in CR3.LAM_U48 later. Logs the live
 * state of CR3 LAM bits and CR4.LAM_SUP so operators can see the
 * snapshot at boot. Called from main.c right after vmm_pku_init. */
void vmm_lam_probe(void);
void vmm_lam_ap_probe(void);

/* Phase 2J — TME / TME-MK (Total Memory Encryption Multi-Key).
 *
 * Intel SDM Vol 3D §15.5. Two MSRs:
 *   IA32_TME_CAPABILITY (0x981) — supported algorithms + max KeyID bits
 *   IA32_TME_ACTIVATE   (0x982) — firmware-programmed activation
 *
 * IA32_TME_ACTIVATE layout:
 *   bit 0   LOCK         — register is RO after firmware sets this
 *   bit 1   TME enable
 *   bits 4-7 TME_POLICY  — encryption algorithm selection
 *   bits 32-35 NUM_KEYID_BITS
 *   bit 36  TME_MK_EN    — Multi-Key extension active
 *
 * When TME-MK is active, the top NUM_KEYID_BITS of every phys address
 * become the KeyID. The CPU reports MAXPHYADDR REDUCED accordingly via
 * CPUID.80000008:EAX, so PMM already sees the smaller usable range.
 *
 * Phase 2J is observe-only: detect, decode, log. Per-region encryption
 * (allocating with a specific KeyID and ORing into PTE phys bits) is
 * the lifecycle follow-up.
 */
#define VMM_MSR_IA32_TME_CAPABILITY  0x981U
#define VMM_MSR_IA32_TME_ACTIVATE    0x982U

#define VMM_TME_ACT_LOCK             (1ULL << 0)
#define VMM_TME_ACT_TME_EN           (1ULL << 1)
#define VMM_TME_ACT_TME_MK_EN        (1ULL << 36)
#define VMM_TME_ACT_KEYID_BITS_MASK  (0xFULL << 32)
#define VMM_TME_ACT_KEYID_BITS_SHIFT 32

void vmm_tme_probe(void);
void vmm_tme_ap_probe(void);

/* Phase 2K — CET (Control-flow Enforcement Technology).
 *
 * Intel SDM Vol 3D §17. Two sub-features (CET-SS Shadow Stack +
 * CET-IBT Indirect Branch Tracking). Both gate on CR4.CET (bit 23).
 *
 *   IA32_S_CET (0x6A2)  — supervisor-side controls
 *   IA32_U_CET (0x6A0)  — user-side controls
 *   IA32_PL0_SSP (0x6A4) / PL1_SSP / PL2_SSP / PL3_SSP — shadow stack pointers
 *
 * Leaf PTE bit 60 = supervisor shadow-stack indicator (Intel SDM Vol 3A
 * §4.5.1 Table 4-19). On a 4 KiB writable PTE with CR4.CET=1 +
 * IA32_S_CET.SH_STK_EN=1, the page is marked supervisor SS. Bit 61 is
 * the user SS analogue.
 *
 * SSP register saved via XSAVE component 11 (XCR0.CET_S / CET_U at
 * bits 11/12 of XCR0).
 *
 * Phase 2K is observe-only foundation: detect, probe, log, expose
 * encoding helpers + reserved namespace. Per-process shadow stack
 * lifecycle (SSP allocate, IRET-on-SS, set CR4.CET) is the follow-up.
 */
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

static inline uint64_t vmm_pte_with_cet_supv_ss(uint64_t pte) {
    return pte | VMM_PTE_CET_SS_SUPV;
}
static inline bool vmm_pte_is_supv_ss(uint64_t pte) {
    return (pte & VMM_PTE_CET_SS_SUPV) != 0;
}

/* Compose KeyID into the upper bits of a phys address. Pure function;
 * caller must ensure `phys` doesn't already use the bits and that the
 * KeyID fits in the platform's NUM_KEYID_BITS window. Phase 2J ships
 * this as a building block; no callers wire it yet. */
static inline uint64_t vmm_phys_with_keyid(uint64_t phys, uint8_t keyid,
                                            uint8_t num_keyid_bits,
                                            uint8_t reduced_maxphyaddr) {
    /* Defensive bound check — Intel SDM caps NUM_KEYID_BITS at 15 and
     * MAXPHYADDR at 52, so the largest physical position is bit 66 in
     * the worst case (15 + 52 = 67), which overflows uint64. Guard so
     * callers passing bogus values get phys back unchanged rather than
     * UB-shifted garbage. */
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
    // Higher-half kernel: 0xFFFFFFFF80000000+ maps to physical 0+
    if (virt >= 0xFFFFFFFF80000000ULL) {
        return virt - 0xFFFFFFFF80000000ULL;
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
vmm_context_t* vmm_create_cabin(uint64_t* cabin_info_phys,
                                uint64_t* pocket_ring_phys,
                                uint64_t* result_ring_phys,
                                uint64_t* touch_ring_phys);
int vmm_map_cabin_info(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_pocket_ring(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_result_ring(vmm_context_t* ctx, uintptr_t phys_page);
int vmm_map_touch_ring(vmm_context_t* ctx, uintptr_t phys_page);

// Translate a user virtual address in a process's page table to a kernel-accessible pointer.
// Walks the process's page tables, resolves the physical address, and returns it as a
// kernel pointer via Pull Map. Returns NULL if the address is not mapped.
//
// LIMITATION: works only within ONE physical page. The user's virtual range may be
// contiguous, but the underlying physical pages need not be — so a multi-page request
// gets clamped to the first page. Callers needing multi-page payloads must use the
// vmm_user_buf_* helpers below, which copy through a kernel-side bounce buffer.
void* vmm_translate_user_addr(vmm_context_t* ctx, uintptr_t user_vaddr, size_t size);

/*
 * Multi-page user buffer in/out via a kmalloc'd kernel bounce buffer.
 *
 *   vmm_user_buf_in        — copy user → fresh kernel buffer (input crates)
 *   vmm_user_buf_alloc_out — allocate empty kernel buffer (output crates)
 *   vmm_user_buf_commit_out— copy kernel buffer → user pages (after fill)
 *   vmm_user_buf_free      — release the kernel buffer
 *
 * Each helper handles arbitrary cross-page user ranges by walking the
 * page table per-page and memcpy'ing chunk by chunk. NULL means a fault
 * mid-walk (unmapped, non-user, or alloc failure).
 */
void *vmm_user_buf_in(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size);

/*
 * vmm_user_buf_in_into — same page-walked copy as vmm_user_buf_in but
 * writes into a caller-supplied kernel buffer (no kmalloc/kfree). Use
 * when the caller already owns a destination (stack scratch, preallocated
 * arena, etc.). Returns OK on success, ERR_INVALID_ARGUMENT on a NULL/0
 * argument, ERR_INVALID_ADDRESS if any page fails to translate.
 */
error_t vmm_user_buf_in_into(vmm_context_t *ctx, uintptr_t user_vaddr,
                              size_t size, void *kbuf);

void   *vmm_user_buf_alloc_out(size_t size);
error_t vmm_user_buf_commit_out(vmm_context_t *ctx, uintptr_t user_vaddr,
                                 const void *kbuf, size_t size);
void    vmm_user_buf_free(void *kbuf);
int vmm_setup_null_trap(vmm_context_t* ctx);
/* Map a process code region.
 *
 *   code_phys / size   physical pages staged with the binary contents.
 *   out_entry          (out) virtual entry point — `ehdr->e_entry` for
 *                       ELF binaries, `VMM_CABIN_CODE_START` for raw
 *                       flat binaries. Caller stores this into the
 *                       process's saved RIP. NULL is permitted only
 *                       for legacy callers that don't care. */
int vmm_map_code_region(vmm_context_t* ctx, uintptr_t code_phys, uint64_t size,
                        uintptr_t *out_entry);

// Called by vmm_init() after Pull Map is live to rebase PMM bitmap pointer
void pmm_activate_pull_map(void);

/* Program IA32_PAT on the calling logical processor. Intel SDM Vol 3A
 * §11.12.4: the PAT MSR is per-logical-processor and MUST be identically
 * programmed on every CPU, otherwise WC framebuffer mappings touched on
 * one core behave as UC- on another core (the reset default). */
void vmm_pat_init(void);

/* ────────────────────────────────────────────────────────────────────────
 * 2 MB huge-page mapping. Single PDE leaf with VMM_FLAG_LARGE_PAGE set.
 *
 * Both `virt_addr` and `phys_addr` MUST be 2 MB-aligned (0x200000). PMM's
 * buddy returns naturally-aligned blocks at order 9 (pmm_alloc(512)) so
 * the typical call sequence is:
 *
 *   void *phys = pmm_alloc(512);            // 2 MB-aligned phys
 *   vmm_map_huge_2m(ctx, va, (uintptr_t)phys, VMM_FLAGS_USER_RW);
 *
 * The VMM_FLAG_LARGE_PAGE bit is added internally; callers pass the same
 * flags they would pass to vmm_map_page(). Returns true on success; false
 * if the PDE is already occupied (CAS lost) or an intermediate PML4/PDPT
 * entry can't be allocated.
 *
 * Single-PDE mapping ⇒ 512 ⇒ one TLB entry covers 2 MB. Used by Bay
 * (cross-cabin shared memory) and user-heap pre-fault path for ≥ 2 MB
 * allocations. NOT used by the kernel boot mappings (those use direct
 * PDE writes in vmm_init).
 * ──────────────────────────────────────────────────────────────────────── */
bool vmm_map_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr,
                     uintptr_t phys_addr, uint64_t flags);

bool vmm_unmap_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr);

/* Resolve a 2 MB-mapped virt_addr to its backing physical 2 MB-aligned
 * address. Returns 0 if the PDE is not present, was demoted to 4 KB, or
 * the VA is not 2 MB-aligned. */
uintptr_t vmm_virt_to_phys_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr);

/* Cross-core full TLB flush — every online core reloads CR3 (NOFLUSH bit
 * cleared). Used by callers that need every PCID partition invalidated. */
void vmm_shootdown_all_cores_full(void);

#endif // VMM_H
