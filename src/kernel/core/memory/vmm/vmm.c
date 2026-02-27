#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "error.h"
#include "../../arch/x86-64/cpu/cpuid.h"
#include "process.h"
#include "cpu_caps_page.h"
#include "boxos_addresses.h"


// ========== GLOBAL VARIABLES ==========
static vmm_context_t* kernel_context = NULL;
static vmm_context_t* current_context = NULL;
static bool vmm_initialized = false;
static char last_error[256] = {0};
static vmm_stats_t global_stats = {0};
static spinlock_t vmm_global_lock = {0};

// MAXPHYADDR detection (initialized in vmm_init)
uint8_t vmm_maxphyaddr = 36;
uint64_t vmm_pte_addr_mask = 0x0000000FFFFFF000ULL;

// Kernel heap tracking
static uintptr_t kernel_heap_current = VMM_KERNEL_HEAP_BASE;
static spinlock_t kernel_heap_lock = {0};

// Kernel MMIO region tracking
static uintptr_t kernel_mmio_current = VMM_KERNEL_MMIO_BASE;
static spinlock_t kernel_mmio_lock = {0};

// vmalloc tracking (simple linked list)
typedef struct vmalloc_entry {
    void* virt_base;
    size_t pages;
    struct vmalloc_entry* next;
} vmalloc_entry_t;

static vmalloc_entry_t* vmalloc_list = NULL;
static spinlock_t vmalloc_lock = {0};

// ========== ERROR HANDLING ==========
void vmm_set_error(const char* error) {
    if (error) {
        strncpy(last_error, error, sizeof(last_error) - 1);
        last_error[sizeof(last_error) - 1] = '\0';
    } else {
        last_error[0] = '\0';
    }
}

const char* vmm_get_last_error(void) {
    return last_error;
}

// ========== PHYSICAL MEMORY INTEGRATION ==========
uintptr_t vmm_alloc_page_table(void) {
    void* page = pmm_alloc_zero(1);  // One page, zero-initialized (returns phys pointer)
    if (!page) {
        vmm_set_error("Failed to allocate page table from PMM");
        return 0;
    }
    spin_lock(&vmm_global_lock);
    global_stats.page_tables_allocated++;
    spin_unlock(&vmm_global_lock);
    return (uintptr_t)page;
}

void vmm_free_page_table(uintptr_t phys_addr) {
    if (!phys_addr) return;
    pmm_free((void*)phys_addr, 1);
    spin_lock(&vmm_global_lock);
    if (global_stats.page_tables_allocated > 0) global_stats.page_tables_allocated--;
    spin_unlock(&vmm_global_lock);
}

// ========== TLB MANAGEMENT ==========
void vmm_flush_tlb(void) {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    spin_lock(&vmm_global_lock);
    global_stats.tlb_flushes++;
    spin_unlock(&vmm_global_lock);
}

void vmm_flush_tlb_page(uintptr_t virt_addr) {
    // BUG #11 DOCUMENTED LIMITATION: TLB shootdown not implemented
    // This only flushes TLB on the current core. On multi-core systems,
    // other cores may have stale TLB entries for this page.
    // TODO: Implement IPI-based TLB shootdown for proper multi-core support
    // For now, BoxOS assumes single-core or careful memory isolation
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    spin_lock(&vmm_global_lock);
    global_stats.tlb_flushes++;
    spin_unlock(&vmm_global_lock);
}

void vmm_invalidate_page(uintptr_t virt_addr) {
    vmm_flush_tlb_page(virt_addr);
}

// ========== PAGE TABLE MANIPULATION (helpers) ==========

// Internal: walk and create intermediate tables up to `level` (1..3). Return pointer to that table (virtual).
// level==1 -> return PDPT, level==2 -> return PD, level==3 -> return PT.
page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level) {
    if (!ctx || !ctx->pml4) return NULL;

    page_table_t* current_table = ctx->pml4;
    uint32_t indices[4] = {
        VMM_PML4_INDEX(virt_addr),
        VMM_PDPT_INDEX(virt_addr),
        VMM_PD_INDEX(virt_addr),
        VMM_PT_INDEX(virt_addr)
    };

    // Walk down to the requested level
    for (int i = 0; i < level; i++) {
        pte_t* entry = &current_table->entries[indices[i]];

        if (!(*entry & VMM_FLAG_PRESENT)) {
            // Need to create new table
            uintptr_t new_table_phys = vmm_alloc_page_table();
            if (!new_table_phys) {
                vmm_set_error("Failed to allocate page table");
                return NULL;
            }

            // Map it with kernel + user flags (intermediate tables need USER bit!)
            // CRITICAL: All levels of page tables must have USER bit for Ring 3 access
            *entry = vmm_make_pte(new_table_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
        } else {
            // Entry exists - check if it's a large page that needs splitting
            // At i=2, current_table is PD, entry is PD[indices[2]]
            // If this entry has LARGE_PAGE bit, it's a 2MB page that must be split
            if (i == 2 && (*entry & VMM_FLAG_LARGE_PAGE)) {
                // BUG #7 FIX: Race condition on multi-core during large page split
                // Two cores could both see LARGE_PAGE bit and allocate duplicate PTs
                // Read entry atomically before split
                pte_t old_entry = *entry;
                if (!(old_entry & VMM_FLAG_LARGE_PAGE)) {
                    // Already split by another core, continue normally
                    goto entry_ready;
                }

                // This is a PD entry with a 2MB large page that needs splitting into 512 4KB pages
                uintptr_t large_page_base = vmm_pte_to_phys(old_entry);
                uint64_t large_page_flags = vmm_pte_to_flags(old_entry) & ~VMM_FLAG_LARGE_PAGE;

                // Allocate new PT for 512 4KB pages
                uintptr_t new_pt_phys = vmm_alloc_page_table();
                if (!new_pt_phys) {
                    vmm_set_error("Failed to allocate PT for large page split");
                    return NULL;
                }

                page_table_t* new_pt = (page_table_t*)vmm_phys_to_virt(new_pt_phys);

                // Fill PT with 512 4KB mappings covering the same 2MB range
                for (int j = 0; j < 512; j++) {
                    uintptr_t page_phys = large_page_base + (j * VMM_PAGE_SIZE);
                    new_pt->entries[j] = vmm_make_pte(page_phys, large_page_flags);
                }

                // Atomically replace large page entry with PT pointer
                // Use CAS to detect if another core already split this page
                pte_t new_entry = vmm_make_pte(new_pt_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
                if (!atomic_cas_u64((volatile uint64_t*)entry, old_entry, new_entry)) {
                    // Another core beat us to it, free our PT and retry
                    vmm_free_page_table(new_pt_phys);
                    // Entry was modified by another core, re-read it
                }
            }
entry_ready:
            ;  // Empty statement for label
        }

        // Move to next level (phys -> virtual pointer)
        // CRITICAL: Using vmm_phys_to_virt() to convert physical to virtual
        // Currently relies on identity mapping (0-2GB), future: proper higher-half mapping
        uintptr_t next_table_phys = vmm_pte_to_phys(*entry);
        current_table = (page_table_t*)vmm_phys_to_virt(next_table_phys);
    }

    return current_table;
}

// Internal: get existing (no allocation) PTE. Returns NULL if PT or parent tables not present.
// BUG #16 DOCUMENTED RACE: Page table walk not atomic against concurrent modifications
// If another core modifies page tables during this walk, we may read inconsistent state.
// This is acceptable for current single-core focus. For multi-core, would need:
// - RCU (Read-Copy-Update) for safe concurrent page table reads
// - Per-context locks held during entire walk
// - Sequence numbers to detect modifications during walk
static pte_t* vmm_get_pte_noalloc(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx || !ctx->pml4) return NULL;

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx   = VMM_PD_INDEX(virt_addr);
    uint32_t pt_idx   = VMM_PT_INDEX(virt_addr);

    page_table_t* pml4 = ctx->pml4;
    pte_t pml4_entry = pml4->entries[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT)) return NULL;

    page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) return NULL;

    // If PDPT entry is large page (1GB), then there's no lower PT
    if (pdpt_entry & VMM_FLAG_LARGE_PAGE) {
        // No PT; represent as PTE not present here
        return NULL;
    }

    page_table_t* pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) return NULL;

    // If PD entry is large page (2MB), there's no PT
    if (pd_entry & VMM_FLAG_LARGE_PAGE) {
        return NULL;
    }

    page_table_t* pt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    return &pt->entries[pt_idx];
}

// Get PTE and create page tables if necessary
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr) {
    page_table_t* pt = vmm_get_or_create_table(ctx, virt_addr, 3);
    if (!pt) return NULL;
    return &pt->entries[VMM_PT_INDEX(virt_addr)];
}

// Public wrappers (as declared in header).
// vmm_get_pte -> does NOT create tables (safe for translations)
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr) {
    return vmm_get_pte_noalloc(ctx, virt_addr);
}



// ========== CONTEXT MANAGEMENT ==========
vmm_context_t* vmm_create_context(void) {
    vmm_context_t* ctx = kmalloc(sizeof(vmm_context_t));
    if (!ctx) {
        vmm_set_error("Failed to allocate VMM context");
        return NULL;
    }

    memset(ctx, 0, sizeof(vmm_context_t));

    // Allocate PML4 table
    uintptr_t pml4_phys = vmm_alloc_page_table();
    if (!pml4_phys) {
        kfree(ctx);
        vmm_set_error("Failed to allocate PML4 table");
        return NULL;
    }

    ctx->pml4 = (page_table_t*)vmm_phys_to_virt(pml4_phys);
    ctx->pml4_phys = pml4_phys;
    ctx->heap_start = VMM_USER_HEAP_BASE;
    ctx->heap_end = VMM_USER_HEAP_BASE;
    ctx->stack_top = VMM_USER_STACK_TOP;

    spinlock_init(&ctx->lock);

    // Copy kernel mappings from kernel_context if we have one
    if (kernel_context && kernel_context->pml4) {

        // CRITICAL FIX: Don't share PDPT for PML4[0]!
        // Problem: User space (0x20000000 = 512MB) is in PML4[0] range (0-512GB)
        // If we copy PML4[0] entry directly, all processes share the same PDPT,
        // causing Process 1's mappings to contaminate Process 2's address space.
        //
        // Solution: Create a NEW PDPT for this context and copy only the
        // identity-mapped large pages (0-256MB) from kernel's PDPT.
        // Allocate new PDPT for this context
        uintptr_t new_pdpt_phys = vmm_alloc_page_table();
        if (!new_pdpt_phys) {
            vmm_free_page_table(pml4_phys);
            kfree(ctx);
            vmm_set_error("Failed to allocate PDPT for new context");
            return NULL;
        }

        page_table_t* new_pdpt = (page_table_t*)vmm_phys_to_virt(new_pdpt_phys);
        // Get kernel's PDPT for PML4[0]

        pte_t kernel_pml4_entry = kernel_context->pml4->entries[0];
        if (kernel_pml4_entry & VMM_FLAG_PRESENT) {
            page_table_t* kernel_pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kernel_pml4_entry));

            // Copy only the large page entries (identity mapping 0-256MB)
            // PDPT entry 0 covers 0-1GB, and within the corresponding PD,
            // entries 0-127 are 2MB large pages covering 0-256MB
            // We need to copy PDPT[0] which points to the PD with large pages

            pte_t kernel_pdpt_entry = kernel_pdpt->entries[0];
            if (kernel_pdpt_entry & VMM_FLAG_PRESENT) {
                // Check if it points to a PD (not a 1GB large page)
                if (!(kernel_pdpt_entry & VMM_FLAG_LARGE_PAGE)) {
                    // Allocate new PD for this context

                    uintptr_t new_pd_phys = vmm_alloc_page_table();
                    if (!new_pd_phys) {
                        vmm_free_page_table(new_pdpt_phys);
                        vmm_free_page_table(pml4_phys);
                        kfree(ctx);
                        vmm_set_error("Failed to allocate PD for identity mapping");
                        return NULL;
                    }

                    page_table_t* new_pd = (page_table_t*)vmm_phys_to_virt(new_pd_phys);
                    page_table_t* kernel_pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kernel_pdpt_entry));

                    // Copy large page entries (0-127 = 0-256MB)

                    for (int i = 0; i < 128; i++) {

                        new_pd->entries[i] = kernel_pd->entries[i];

                    }

                    // Entries 128-511 (256MB-2GB) remain zero (unmapped in new context)

                    // Set new PD in new PDPT
                    new_pdpt->entries[0] = vmm_make_pte(new_pd_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
                }
            }
        }

        // Set new PDPT in PML4[0]
        ctx->pml4->entries[0] = vmm_make_pte(new_pdpt_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);


        // Copy upper half (kernel space indexes 256..511) - these are safe to share
        for (int i = 256; i < 512; i++) {
            ctx->pml4->entries[i] = kernel_context->pml4->entries[i];
        }
    }

    spin_lock(&vmm_global_lock);
    global_stats.total_contexts++;
    spin_unlock(&vmm_global_lock);

    return ctx;
}

// Helper: free user-space page tables & mapped pages for a context
static void vmm_free_user_space_tables(vmm_context_t* ctx) {
    if (!ctx || !ctx->pml4) return;

    debug_printf("[VMM] Freeing user space tables for context CR3=0x%lx\n", ctx->pml4_phys);

    // CRITICAL FIX (2026-01-31): Track freed pages to prevent double-free
    // Allocate a bitmap to track which physical pages we've already freed
    // Assuming max 128MB of user pages (32768 pages = 4096 bytes bitmap)
    #define MAX_USER_PAGES 32768
    #define BITMAP_SIZE (MAX_USER_PAGES / 8)

    uint8_t* freed_bitmap = kmalloc(BITMAP_SIZE);
    if (!freed_bitmap) {
        debug_printf("[VMM] ERROR: Failed to allocate freed page bitmap, skipping cleanup\n");
        return;
    }
    memset(freed_bitmap, 0, BITMAP_SIZE);

    // Only iterate low half (user space): PML4 indices 0..255
        // NOTE: After the fix, PML4[0] has its own PDPT/PD per context (not shared)

    // We still skip freeing kernel identity-mapped large pages (0-256MB)

    for (int p4 = 0; p4 < 256; p4++) {

        pte_t pml4_entry = ctx->pml4->entries[p4];
        if (!(pml4_entry & VMM_FLAG_PRESENT)) continue;

        uintptr_t pdpt_phys = vmm_pte_to_phys(pml4_entry);
        page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(pdpt_phys);

        // Each context has its own PDPT and PD for PML4[0]
        // However, PT structures may be shared if created by splitting large pages
        // We should free PDPT and PD (they're per-context), but NOT the PT (it may be shared)
        bool is_identity_pml4_entry = (p4 == 0);
        int skip_pt_free = is_identity_pml4_entry ? 1 : 0;

        // iterate PDPT entries
        for (int p3 = 0; p3 < 512; p3++) {
            pte_t pdpt_entry = pdpt->entries[p3];
            if (!(pdpt_entry & VMM_FLAG_PRESENT)) continue;

            // SKIP 1GB large pages - these are kernel mappings
            if (pdpt_entry & VMM_FLAG_LARGE_PAGE) {
                continue;  // Skip kernel large pages
            }

            uintptr_t pd_phys = vmm_pte_to_phys(pdpt_entry);
            page_table_t* pd = (page_table_t*)vmm_phys_to_virt(pd_phys);

            // iterate PD entries
            for (int p2 = 0; p2 < 512; p2++) {
                pte_t pd_entry = pd->entries[p2];
                if (!(pd_entry & VMM_FLAG_PRESENT)) continue;

                // SKIP 2MB large pages - these are kernel identity mappings
                if (pd_entry & VMM_FLAG_LARGE_PAGE) {
                    continue;  // Skip kernel large pages
                }

                if (is_identity_pml4_entry) {
                    vmm_context_t* kern = vmm_get_kernel_context();
                    if (kern && kern->pml4) {
                        pte_t kern_pml4e = kern->pml4->entries[0];
                        if (kern_pml4e & VMM_FLAG_PRESENT) {
                            page_table_t* kern_pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kern_pml4e));
                            pte_t kern_pdpte = kern_pdpt->entries[p3];
                            if ((kern_pdpte & VMM_FLAG_PRESENT) && !(kern_pdpte & VMM_FLAG_LARGE_PAGE)) {
                                page_table_t* kern_pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kern_pdpte));
                                pte_t kern_pd_entry = kern_pd->entries[p2];
                                if ((kern_pd_entry & VMM_FLAG_PRESENT) &&
                                    vmm_pte_to_phys(pd_entry) == vmm_pte_to_phys(kern_pd_entry)) {
                                    continue;  // Shared kernel PT — do NOT touch
                                }
                            }
                        }
                    }
                }

                uintptr_t pt_phys = vmm_pte_to_phys(pd_entry);
                page_table_t* pt = (page_table_t*)vmm_phys_to_virt(pt_phys);

                // iterate PT entries (leaf pages) - FREE DATA PAGES
                int freed_pages = 0;
                for (int p1 = 0; p1 < 512; p1++) {
                    pte_t pt_entry = pt->entries[p1];
                    if (!(pt_entry & VMM_FLAG_PRESENT)) continue;

                    uintptr_t phys = vmm_pte_to_phys(pt_entry);

                    // Calculate virtual address for identity mapping check
                    uintptr_t virt = (p4 * 512ULL * 1024 * 1024 * 1024) +
                                     (p3 * 1024 * 1024 * 1024) +
                                     (p2 * 2 * 1024 * 1024) +
                                     (p1 * 4096);
                    bool is_identity_mapped = (phys == virt);

                    extern uint64_t g_cpu_caps_page_phys;
                    if (phys == g_cpu_caps_page_phys) {
                        if (!is_identity_mapped) {
                            pt->entries[p1] = 0;
                        }
                        continue;
                    }

                    // CRITICAL FIX: Check if we've already freed this page
                    if (!is_identity_mapped && phys >= 0x100000 && phys < 0x20000000) {
                        // Calculate bitmap index (page number relative to 1MB)
                        size_t page_idx = (phys - 0x100000) / VMM_PAGE_SIZE;

                        if (page_idx < MAX_USER_PAGES) {
                            size_t byte_idx = page_idx / 8;
                            size_t bit_idx = page_idx % 8;

                            // Check if already freed
                            if (freed_bitmap[byte_idx] & (1 << bit_idx)) {
                                debug_printf("[VMM]   SKIP: Page 0x%lx already freed (duplicate PT entry at virt=0x%lx)\n", phys, virt);
                            } else {
                                freed_bitmap[byte_idx] |= (1 << bit_idx);
                                debug_printf("[VMM]   FREE: Page 0x%lx (virt=0x%lx, PT entry %d)\n", phys, virt, p1);
                                pmm_free((void*)phys, 1);
                                freed_pages++;
                            }
                        } else {
                            debug_printf("[VMM]   WARNING: Page 0x%lx outside bitmap range\n", phys);
                        }
                    }

                    if (!is_identity_mapped) {
                        pt->entries[p1] = 0;
                    }
                }

                if (freed_pages > 0) {
                    debug_printf("[VMM]   Freed %d data pages from PT\n", freed_pages);
                }

                // Free PT table itself ONLY if not shared with kernel identity mapping
                // For PML4[0], PTs may have been created by splitting large pages
                // These are shared across all contexts and must NOT be freed
                // Note: PTs are NOT shared in current implementation
                // Each process has its own PT copies
                // skip_pt_free flag only applies to identity-mapped kernel regions
                if (!skip_pt_free) {
                    vmm_free_page_table(pt_phys);
                }
                // Always zero the PD entry to unlink the PT
                pd->entries[p2] = 0;
            }

            // Free PD table (it's per-context, not shared)
            vmm_free_page_table(pd_phys);
            pdpt->entries[p3] = 0;
        }

        // Free PDPT table (it's per-context, not shared)
        vmm_free_page_table(pdpt_phys);
        ctx->pml4->entries[p4] = 0;
    }

    kfree(freed_bitmap);
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    #undef MAX_USER_PAGES
    #undef BITMAP_SIZE

    debug_printf("[VMM] User space tables freed\n");
}

void vmm_destroy_context(vmm_context_t* ctx) {
    if (!ctx || ctx == kernel_context) return;

    uintptr_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(saved_cr3));

    // ALWAYS switch to kernel CR3 before walking/freeing page tables.
    // User contexts have split PD entries (2MB large pages broken into 4KB PTs)
    // which may not identity-map all physical addresses used by other contexts'
    // page table structures. The kernel context has pristine 2MB identity mapping.
    uintptr_t destroyed_pml4 = ctx->pml4_phys;
    if (saved_cr3 != kernel_context->pml4_phys) {
        asm volatile("mov %0, %%cr3" :: "r"(kernel_context->pml4_phys) : "memory");
    }

    spin_lock(&ctx->lock);

    vmm_free_user_space_tables(ctx);

    if (ctx->pml4_phys) {
        vmm_free_page_table(ctx->pml4_phys);
    }

    spin_unlock(&ctx->lock);

    kfree(ctx);

    // Restore original CR3 unless it was the context we just destroyed
    if (saved_cr3 != destroyed_pml4 && saved_cr3 != kernel_context->pml4_phys) {
        asm volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    }

    spin_lock(&vmm_global_lock);
    if (global_stats.total_contexts > 0) global_stats.total_contexts--;
    spin_unlock(&vmm_global_lock);
}

vmm_context_t* vmm_get_kernel_context(void) {
    return kernel_context;
}

vmm_context_t* vmm_get_current_context(void) {
    return current_context ? current_context : kernel_context;
}

void vmm_switch_context(vmm_context_t* ctx) {
    if (!ctx || !ctx->pml4_phys) return;

    current_context = ctx;
    asm volatile("mov %0, %%cr3" : : "r"(ctx->pml4_phys) : "memory");
    vmm_flush_tlb();
}

// ========== MEMORY MAPPING ==========
vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags) {
    vmm_map_result_t result = {0};

    if (!ctx) {
        result.error_msg = "Invalid context";
        return result;
    }

    if (!vmm_is_page_aligned(virt_addr) || !vmm_is_page_aligned(phys_addr)) {
        result.error_msg = "Address not page-aligned";
        return result;
    }

    spin_lock(&ctx->lock);

    // We need to create page table for mapping
    pte_t* pte = vmm_get_or_create_pte(ctx, virt_addr);
    if (!pte) {
        spin_unlock(&ctx->lock);
        result.error_msg = "Failed to get/create page table entry";
        return result;
    }

    // Check if page is already mapped
    if (*pte & VMM_FLAG_PRESENT) {
        uintptr_t existing_phys = vmm_pte_to_phys(*pte);
        uint64_t existing_flags = vmm_pte_to_flags(*pte);

        // If exact same mapping, success
        if (existing_phys == phys_addr && existing_flags == flags) {
            spin_unlock(&ctx->lock);
            result.success = true;
            result.virt_addr = virt_addr;
            result.phys_addr = phys_addr;
            result.pages_mapped = 1;
            return result;
        }

        // Allow remapping if it's an identity mapping from large page split
        // Identity mapping: phys == virt and kernel-only flags
        bool is_identity_map = (existing_phys == virt_addr) &&
                              !(existing_flags & VMM_FLAG_USER);

        if (is_identity_map) {
            // Remap is allowed - this is replacing identity mapping with user mapping
            // Fall through to remap
        } else {
            // Not identity mapping - conflict!
            spin_unlock(&ctx->lock);
            char error_buf[256];
            ksnprintf(error_buf, sizeof(error_buf),
                      "Page already mapped (virt=0x%p: existing_phys=0x%p, new_phys=0x%p, existing_flags=0x%llx, new_flags=0x%llx)",
                      (void*)virt_addr, (void*)existing_phys, (void*)phys_addr,
                      (unsigned long long)existing_flags, (unsigned long long)flags);
            debug_printf("[VMM] %s\n", error_buf);
            result.error_msg = "Page already mapped with different address/flags";
            return result;
        }
    }

    // Map the page
    *pte = vmm_make_pte(phys_addr, flags);

    // Update statistics
    ctx->mapped_pages++;
    if (flags & VMM_FLAG_USER) {
        ctx->user_pages++;
        spin_lock(&vmm_global_lock);
        global_stats.user_mapped_pages++;
        global_stats.total_mapped_pages++;
        spin_unlock(&vmm_global_lock);
    } else {
        ctx->kernel_pages++;
        spin_lock(&vmm_global_lock);
        global_stats.kernel_mapped_pages++;
        global_stats.total_mapped_pages++;
        spin_unlock(&vmm_global_lock);
    }

    spin_unlock(&ctx->lock);

    // Invalidate TLB for this page
    vmm_flush_tlb_page(virt_addr);

    result.success = true;
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;
    result.pages_mapped = 1;

    return result;
}

vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags) {
    vmm_map_result_t result = {0};
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;

    for (size_t i = 0; i < page_count; i++) {
        vmm_map_result_t single_result = vmm_map_page(ctx,
            virt_addr + i * VMM_PAGE_SIZE,
            phys_addr + i * VMM_PAGE_SIZE,
            flags);

        if (!single_result.success) {
            // Rollback previous mappings
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(ctx, virt_addr + j * VMM_PAGE_SIZE);
            }
            result.error_msg = single_result.error_msg;
            return result;
        }

        result.pages_mapped++;
    }

    result.success = true;
    return result;
}

bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx || !vmm_is_page_aligned(virt_addr)) return false;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return false;
    }

    // Update statistics (only counts, do not free physical pages here)
    uint64_t flags = vmm_pte_to_flags(*pte);
    if (flags & VMM_FLAG_USER) {
        if (ctx->user_pages > 0) ctx->user_pages--;
        spin_lock(&vmm_global_lock);
        if (global_stats.user_mapped_pages > 0) global_stats.user_mapped_pages--;
        if (global_stats.total_mapped_pages > 0) global_stats.total_mapped_pages--;
        spin_unlock(&vmm_global_lock);
    } else {
        if (ctx->kernel_pages > 0) ctx->kernel_pages--;
        spin_lock(&vmm_global_lock);
        if (global_stats.kernel_mapped_pages > 0) global_stats.kernel_mapped_pages--;
        if (global_stats.total_mapped_pages > 0) global_stats.total_mapped_pages--;
        spin_unlock(&vmm_global_lock);
    }

    if (ctx->mapped_pages > 0) ctx->mapped_pages--;

    // Clear the PTE
    *pte = 0;

    spin_unlock(&ctx->lock);

    // Invalidate TLB
    vmm_flush_tlb_page(virt_addr);

    return true;
}

bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count) {
    bool success = true;

    for (size_t i = 0; i < page_count; i++) {
        if (!vmm_unmap_page(ctx, virt_addr + i * VMM_PAGE_SIZE)) {
            success = false;
        }
    }

    return success;
}

// ========== MMIO MAPPING ==========
volatile void* vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags) {
    if (size == 0) {
        vmm_set_error("vmm_map_mmio: size is zero");
        return NULL;
    }

    // Check if physical address range overlaps USABLE RAM from E820 map
    // This correctly allows MMIO regions (EBDA, BIOS ROM, PCI MMIO) while
    // blocking accidental mapping of managed RAM
    if (pmm_is_usable_ram(phys_addr, size)) {
        vmm_set_error("vmm_map_mmio: physical address overlaps USABLE RAM");
        debug_printf("[VMM] ERROR: vmm_map_mmio phys=0x%llx size=0x%llx overlaps USABLE RAM from E820\n",
                     phys_addr, (uint64_t)size);
        debug_printf("[VMM]        MMIO mappings cannot overlap E820_USABLE regions\n");
        return NULL;
    }

    // Align to page boundaries
    uintptr_t phys_aligned = vmm_page_align_down(phys_addr);
    size_t offset = phys_addr - phys_aligned;
    size_t size_aligned = vmm_page_align_up(size + offset);
    size_t page_count = size_aligned / VMM_PAGE_SIZE;

    // Allocate virtual address space from kernel MMIO region
    spin_lock(&kernel_mmio_lock);

    uintptr_t virt_base = kernel_mmio_current;

    // Check if we have enough space
    if (virt_base + size_aligned > VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE) {
        spin_unlock(&kernel_mmio_lock);
        vmm_set_error("vmm_map_mmio: kernel MMIO region exhausted");
        debug_printf("[VMM] ERROR: MMIO region exhausted (need %zu bytes)\n", size_aligned);
        return NULL;
    }

    kernel_mmio_current += size_aligned;
    spin_unlock(&kernel_mmio_lock);

    // Ensure cache-disable and write-through for MMIO
    uint64_t mmio_flags = flags | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;

    // Map pages
    vmm_context_t* ctx = vmm_get_kernel_context();
    vmm_map_result_t result = vmm_map_pages(ctx, virt_base, phys_aligned,
                                            page_count, mmio_flags);

    if (!result.success) {
        debug_printf("[VMM] ERROR: vmm_map_mmio: failed to map pages: %s\n", result.error_msg);
        // Roll back virtual address allocation
        spin_lock(&kernel_mmio_lock);
        if (kernel_mmio_current == virt_base + size_aligned) {
            kernel_mmio_current = virt_base;
        }
        spin_unlock(&kernel_mmio_lock);
        return NULL;
    }

    volatile void* virt_addr = (volatile void*)(virt_base + offset);

    return virt_addr;
}

void vmm_unmap_mmio(volatile void* virt_addr, size_t size) {
    if (!virt_addr || size == 0) return;

    void* virt = (void*)virt_addr;
    uintptr_t virt_base = vmm_page_align_down((uintptr_t)virt);
    size_t size_aligned = vmm_page_align_up(size);
    size_t page_count = size_aligned / VMM_PAGE_SIZE;

    debug_printf("[VMM] vmm_unmap_mmio: virt=%p size=0x%llx pages=%zu\n",
                 virt, size, page_count);

    vmm_context_t* ctx = vmm_get_kernel_context();
    vmm_unmap_pages(ctx, virt_base, page_count);

    // NOTE: We don't reclaim virtual address space (simple bump allocator)
    // For production, implement proper virtual address allocator with free list
}

// ========== HIGH-LEVEL ALLOCATION ==========
void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags) {
    if (!ctx || page_count == 0) {
        debug_printf("[VMM] vmm_alloc_pages: invalid parameters (ctx=%p, count=%zu)\n", ctx, page_count);
        return NULL;
    }

    debug_printf("[VMM] vmm_alloc_pages: requesting %zu pages with flags 0x%llx\n", page_count, (unsigned long long)flags);

    // Allocate physical pages first (returns pointer to physical memory)
    void* phys_pages = pmm_alloc(page_count);
    if (!phys_pages) {
        vmm_set_error("Failed to allocate physical pages");
        debug_printf("[VMM] PMM allocation failed for %zu pages\n", page_count);
        return NULL;
    }

    debug_printf("[VMM] PMM allocated %zu pages at physical 0x%p\n", page_count, phys_pages);

    uintptr_t phys_base = (uintptr_t)phys_pages;
    uintptr_t virt_base;

    // Find virtual address space
    if (flags & VMM_FLAG_USER) {
        virt_base = vmm_find_free_region(ctx, vmm_pages_to_size(page_count),
                                         VMM_USER_BASE, VMM_USER_STACK_TOP);
        if (!virt_base) {
            pmm_free(phys_pages, page_count);
            vmm_set_error("Failed to find user virtual address space");
            debug_printf("[VMM] Failed to find user virtual space for %zu pages\n", page_count);
            return NULL;
        }
        debug_printf("[VMM] Found user virtual space at 0x%p\n", (void*)virt_base);
    } else {
        // Kernel allocation - use simple sequential allocation
        spin_lock(&kernel_heap_lock);
        virt_base = kernel_heap_current;

        debug_printf("[VMM] Current kernel heap pointer: 0x%p\n", (void*)kernel_heap_current);
        debug_printf("[VMM] Kernel heap base: 0x%p\n", (void*)VMM_KERNEL_HEAP_BASE);
        debug_printf("[VMM] Kernel heap size: 0x%llx\n", (unsigned long long)VMM_KERNEL_HEAP_SIZE);

        // Check if we have enough space (basic check)
        if (virt_base + vmm_pages_to_size(page_count) > VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE) {
            spin_unlock(&kernel_heap_lock);
            pmm_free(phys_pages, page_count);
            vmm_set_error("Kernel heap exhausted");
            debug_printf("[VMM] ERROR: Kernel heap exhausted! Current: 0x%p, need: 0x%llx, limit: 0x%p\n",
                   (void*)virt_base, (unsigned long long)vmm_pages_to_size(page_count),
                   (void*)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
            return NULL;
        }

        kernel_heap_current += vmm_pages_to_size(page_count);
        spin_unlock(&kernel_heap_lock);

        debug_printf("[VMM] Kernel allocation: virt=0x%p, phys=0x%p, pages=%zu\n",
               (void*)virt_base, (void*)phys_base, page_count);
    }

    // Map the pages individually for better error handling
    for (size_t i = 0; i < page_count; i++) {
        uintptr_t virt_addr = virt_base + i * VMM_PAGE_SIZE;
        uintptr_t phys_addr = phys_base + i * VMM_PAGE_SIZE;

        debug_printf("[VMM] Mapping page %zu/%zu: virt=0x%p -> phys=0x%p\n",
               i + 1, page_count, (void*)virt_addr, (void*)phys_addr);

        vmm_map_result_t result = vmm_map_page(ctx, virt_addr, phys_addr, flags);

        if (!result.success) {
            debug_printf("[VMM] ERROR: Failed to map page %zu/%zu (virt=0x%p, phys=0x%p): %s\n",
                   i + 1, page_count, (void*)virt_addr, (void*)phys_addr,
                   result.error_msg ? result.error_msg : "unknown error");

            // Rollback previous mappings
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(ctx, virt_base + j * VMM_PAGE_SIZE);
            }

            pmm_free(phys_pages, page_count);
            vmm_set_error(result.error_msg);
            return NULL;
        }
    }

    debug_printf("[VMM] SUCCESS: Allocated %zu pages at virtual 0x%p\n", page_count, (void*)virt_base);
    return (void*)virt_base;
}

void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count) {
    if (!ctx || !virt_addr || page_count == 0) return;

    uintptr_t virt_base = (uintptr_t)virt_addr;

    // Collect physical addresses
    uintptr_t* phys_addrs = kmalloc(page_count * sizeof(uintptr_t));
    if (phys_addrs) {
        for (size_t i = 0; i < page_count; i++) {
            phys_addrs[i] = vmm_virt_to_phys(ctx, virt_base + i * VMM_PAGE_SIZE);
        }
    }

    // Unmap virtual pages (does not free physical)
    vmm_unmap_pages(ctx, virt_base, page_count);

    // Free physical pages
    if (phys_addrs) {
        for (size_t i = 0; i < page_count; i++) {
            if (phys_addrs[i]) {
                pmm_free((void*)phys_addrs[i], 1);
            }
        }
        kfree(phys_addrs);
    }
}

// ========== KERNEL HEAP (vmalloc) ==========
void* vmalloc(size_t size) {
    if (size == 0) {
        debug_printf("[VMM] vmalloc: size is 0\n");
        return NULL;
    }

    if (!vmm_initialized) {
        debug_printf("[VMM] vmalloc: VMM not initialized\n");
        return NULL;
    }

    size_t page_count = vmm_size_to_pages(size);
    debug_printf("[VMM] vmalloc: requested %zu bytes (%zu pages)\n", size, page_count);

    vmm_context_t* ctx = vmm_get_current_context();
    if (!ctx) {
        debug_printf("[VMM] vmalloc: no current context\n");
        return NULL;
    }

    void* virt = vmm_alloc_pages(ctx, page_count, VMM_FLAGS_KERNEL_RW);
    if (!virt) {
        debug_printf("[VMM] vmalloc FAILED: %s\n", vmm_get_last_error());
        return NULL;
    }

    uintptr_t phys_first = vmm_virt_to_phys(ctx, (uintptr_t)virt);
    debug_printf("[VMM] vmalloc: allocated virt=%p phys=%p pages=%zu\n",
            virt, (void*)phys_first, page_count);

    // Register allocation for vfree
    vmalloc_entry_t* ent = kmalloc(sizeof(vmalloc_entry_t));
    if (ent) {
        ent->virt_base = virt;
        ent->pages = page_count;
        spin_lock(&vmalloc_lock);
        ent->next = vmalloc_list;
        vmalloc_list = ent;
        spin_unlock(&vmalloc_lock);
        debug_printf("[VMM] vmalloc: recorded allocation (%p, %zu pages)\n", virt, page_count);
    } else {
        debug_printf("[VMM] vmalloc: WARNING: could not record allocation for vfree()\n");
    }

    debug_printf("[VMM] vmalloc SUCCESS: %p (%zu pages)\n", virt, page_count);
    return virt;
}


void* vzalloc(size_t size) {
    void* addr = vmalloc(size);
    if (addr) {
        memset(addr, 0, size);
    }
    return addr;
}

void vfree(void* addr) {
    if (!addr) {
        debug_printf("[VMM] vfree: null pointer, nothing to free\n");
        return;
    }

    spin_lock(&vmalloc_lock);
    vmalloc_entry_t* prev = NULL;
    vmalloc_entry_t* cur = vmalloc_list;

    while (cur) {
        if (cur->virt_base == addr) {
            // Found allocation
            if (prev) prev->next = cur->next;
            else vmalloc_list = cur->next;
            spin_unlock(&vmalloc_lock);

            debug_printf("[VMM] vfree: freeing allocation at %p (%zu pages)\n",
                    addr, cur->pages);

            vmm_free_pages(vmm_get_current_context(), addr, cur->pages);
            kfree(cur);

            debug_printf("[VMM] vfree: successfully freed %p\n", addr);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    spin_unlock(&vmalloc_lock);

    // Not found — fallback mode
    debug_printf("[VMM] vfree: allocation not found in list, fallback free at %p\n", addr);

    uintptr_t phys = vmm_virt_to_phys(vmm_get_current_context(), (uintptr_t)addr);
    if (phys) {
        debug_printf("[VMM] vfree: unmapped single page virt=%p phys=%p\n", addr, (void*)phys);
        vmm_unmap_page(vmm_get_current_context(), (uintptr_t)addr);
        pmm_free((void*)phys, 1);
    } else {
        debug_printf("[VMM] vfree: WARNING: could not resolve physical address for %p\n", addr);
    }
}


// ========== ADDRESS TRANSLATION ==========
uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return 0;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uintptr_t phys_base = vmm_pte_to_phys(*pte);
    uintptr_t offset = virt_addr & VMM_PAGE_OFFSET_MASK;

    spin_unlock(&ctx->lock);

    return phys_base + offset;
}

bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr) {
    return vmm_virt_to_phys(ctx, virt_addr) != 0;
}

uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return 0;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uint64_t flags = vmm_pte_to_flags(*pte);
    spin_unlock(&ctx->lock);

    return flags;
}

// ========== UTILITIES ==========
uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end) {
    if (!ctx || size == 0 || start >= end) return 0;

    size_t pages_needed = vmm_size_to_pages(size);
    uintptr_t current = vmm_page_align_up(start);

    while (current + vmm_pages_to_size(pages_needed) <= end) {
        bool region_free = true;

        // Check pages
        for (size_t i = 0; i < pages_needed; i++) {
            if (vmm_is_mapped(ctx, current + i * VMM_PAGE_SIZE)) {
                region_free = false;
                // skip past this mapped page
                uintptr_t next = current + (i + 1) * VMM_PAGE_SIZE;

                // Check for overflow and ensure we're making progress
                if (next <= current || next >= end) {
                    return 0;  // No free region found (overflow or out of bounds)
                }

                current = vmm_page_align_up(next);
                break;
            }
        }

        if (region_free) {
            return current;
        }
    }

    return 0;
}

bool vmm_is_kernel_addr(uintptr_t addr) {
    return addr >= VMM_KERNEL_BASE;
}

bool vmm_is_user_accessible(uintptr_t virt_addr) {
    vmm_context_t* ctx = vmm_get_current_context();
    if (!ctx) return false;

    uint64_t flags = vmm_get_page_flags(ctx, virt_addr);
    return (flags & VMM_FLAG_USER) != 0;
}

bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags) {
    if (!ctx || size == 0) return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t current_addr = vmm_page_align_down(virt_addr);

    spin_lock(&ctx->lock);

    for (size_t i = 0; i < page_count; i++) {
        pte_t* pte = vmm_get_pte(ctx, current_addr); // noalloc
        if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
            spin_unlock(&ctx->lock);
            return false;
        }

        // Update flags while preserving physical address
        uintptr_t phys_addr = vmm_pte_to_phys(*pte);
        // Ensure present bit remains set unless new_flags explicitly clears it
        uint64_t flags_to_set = (new_flags & VMM_PTE_FLAGS_MASK);
        if (!(flags_to_set & VMM_FLAG_PRESENT)) flags_to_set |= VMM_FLAG_PRESENT;
        *pte = vmm_make_pte(phys_addr, flags_to_set);

        vmm_flush_tlb_page(current_addr);
        current_addr += VMM_PAGE_SIZE;
    }

    spin_unlock(&ctx->lock);
    return true;
}

bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags) {
    if (!ctx || size == 0) return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t virt_base = vmm_page_align_down(start);

    // Allocate physical pages
    void* phys_pages = pmm_alloc(page_count);
    if (!phys_pages) return false;

    // Map the region
    vmm_map_result_t result = vmm_map_pages(ctx, virt_base, (uintptr_t)phys_pages,
                                           page_count, flags);

    if (!result.success) {
        pmm_free(phys_pages, page_count);
        return false;
    }

    return true;
}

// ========== INITIALIZATION ==========

// Initialize MAXPHYADDR detection
static void vmm_init_maxphyaddr(void) {
    vmm_maxphyaddr = cpuid_get_maxphyaddr();

    // Calculate address mask based on MAXPHYADDR
    if (vmm_maxphyaddr >= 52) {
        vmm_pte_addr_mask = 0x000FFFFFFFFFF000ULL;
    } else {
        uint64_t max_addr = (1ULL << vmm_maxphyaddr);
        vmm_pte_addr_mask = (max_addr - 1) & 0xFFFFFFFFFFFFF000ULL;
    }

    debug_printf("[VMM] MAXPHYADDR detection:\n");
    debug_printf("[VMM]   Physical address bits: %u\n", vmm_maxphyaddr);
    debug_printf("[VMM]   Max physical address: 0x%llx (%llu MB)\n",
           (1ULL << vmm_maxphyaddr) - 1,
           (1ULL << vmm_maxphyaddr) / (1024 * 1024));
    debug_printf("[VMM]   PTE address mask: 0x%016llx\n", vmm_pte_addr_mask);

    uint8_t virt_bits = cpuid_get_maxvirtaddr();
    debug_printf("[VMM]   Virtual address bits: %u\n", virt_bits);
    debug_printf("[VMM]   Max virtual address: 0x%llx\n",
           (1ULL << virt_bits) - 1);
}

void vmm_init(void) {
    if (vmm_initialized) {
        debug_printf("[VMM] Already initialized!\n");
        return;
    }

    debug_printf("[VMM] Initializing Virtual Memory Manager...\n");

    // CRITICAL: Detect MAXPHYADDR BEFORE creating any page tables
    vmm_init_maxphyaddr();

    spinlock_init(&vmm_global_lock);
    spinlock_init(&kernel_heap_lock);
    spinlock_init(&vmalloc_lock);
    spinlock_init(&kernel_mmio_lock);

    // Create kernel context
    kernel_context = vmm_create_context();
    if (!kernel_context) {
        panic("Failed to create kernel VMM context");
    }

    debug_printf("[VMM] Kernel context created at %p\n", kernel_context);
    debug_printf("[VMM] PML4 physical address: 0x%p\n", (void*)kernel_context->pml4_phys);

    // Set up identity mapping for ALL usable RAM from E820
    // CRITICAL: VMM relies on physical = virtual for page table access!
    // CRITICAL FIX: Previously limited to 256MB, but PMM can allocate pages beyond
    // this limit if system has >256MB RAM, causing page faults and crashes.
    //
    // OPTIMIZATION: Use 2MB large pages for identity mapping
    // This reduces mappings dramatically, speeding up initialization

    // Get total usable memory from PMM
    uint64_t total_mem = pmm_get_total_memory();

    // Round up to next 2MB boundary
    uintptr_t identity_map_end = ALIGN_UP(total_mem, 2 * 1024 * 1024);

    debug_printf("[VMM] Setting up identity mapping for 0x0 - 0x%llx (%llu MB) using 2MB large pages...\n",
            identity_map_end, identity_map_end / (1024*1024));

    // Map using 2MB large pages for speed
    #define LARGE_PAGE_SIZE (2 * 1024 * 1024)  // 2MB
    size_t large_pages_mapped = 0;

    for (uintptr_t addr = 0; addr < identity_map_end; addr += LARGE_PAGE_SIZE) {
        // Get or create page directory (PD) entry
        page_table_t* pd = vmm_get_or_create_table(kernel_context, addr, 2);
        if (!pd) {
            if (addr < 0x1000000) { // First 16MB is critical
                panic("Failed to create page directory for identity mapping");
            }
            continue;
        }

        // Calculate PD index
        uint32_t pd_idx = VMM_PD_INDEX(addr);
        pte_t* pd_entry = &pd->entries[pd_idx];

        // Create 2MB large page entry (skip Page Table level)
        // Set LARGE_PAGE bit (bit 7) to indicate 2MB page
        *pd_entry = vmm_make_pte(addr, VMM_FLAGS_KERNEL_RW | VMM_FLAG_LARGE_PAGE);
        large_pages_mapped++;
    }

    debug_printf("[VMM] Identity mapped %zu large pages (%zu MB)\n",
           large_pages_mapped, large_pages_mapped * 2);

    debug_printf("[VMM] Kernel heap will be mapped on demand starting at 0x%p\n",
           (void*)VMM_KERNEL_HEAP_BASE);

    debug_printf("[VMM] Kernel MMIO region: %p - %p (on-demand)\n",
                 (void*)VMM_KERNEL_MMIO_BASE,
                 (void*)(VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE));

    // Test identity mapping by writing & reading known physical address (1MB mark)
    debug_printf("[VMM] Testing identity mapping...\n");
    volatile uint32_t* test_ptr = (volatile uint32_t*)0x100000; // 1MB
    uint32_t old_value = *test_ptr;
    *test_ptr = 0xDEADBEEF;
    if (*test_ptr != 0xDEADBEEF) {
        panic("Identity mapping test failed");
    }
    *test_ptr = old_value; // Restore
    debug_printf("[VMM] Identity mapping test: PASSED\n");

    // Switch to our new page tables
    current_context = kernel_context;
    vmm_switch_context(kernel_context);

    // Test that we can still access memory after switch
    *test_ptr = 0xCAFEBABE;
    if (*test_ptr != 0xCAFEBABE) {
        panic("Page table switch broke memory access");
    }
    *test_ptr = old_value; // Restore

    vmm_initialized = true;

    debug_printf("[VMM] Virtual memory layout:\n");
    debug_printf("[VMM]   Kernel base:      0x%p\n", (void*)VMM_KERNEL_BASE);
    debug_printf("[VMM]   Kernel heap:      0x%p - 0x%p (on-demand)\n",
           (void*)VMM_KERNEL_HEAP_BASE,
           (void*)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
    debug_printf("[VMM]   User base:        0x%p\n", (void*)VMM_USER_BASE);
    debug_printf("[VMM]   User heap:        0x%p\n", (void*)VMM_USER_HEAP_BASE);
    debug_printf("[VMM]   User stack top:   0x%p\n", (void*)VMM_USER_STACK_TOP);

    debug_printf("[VMM] %[S]Virtual Memory Manager initialized successfully!%[D]\n");
}

// ========== DEBUGGING & STATISTICS ==========
void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return;

    debug_printf("[VMM] Page table dump for virtual address 0x%p:\n", (void*)virt_addr);
    debug_printf("[VMM]   PML4 index: %d\n", VMM_PML4_INDEX(virt_addr));
    debug_printf("[VMM]   PDPT index: %d\n", VMM_PDPT_INDEX(virt_addr));
    debug_printf("[VMM]   PD index:   %d\n", VMM_PD_INDEX(virt_addr));
    debug_printf("[VMM]   PT index:   %d\n", VMM_PT_INDEX(virt_addr));

    spin_lock(&ctx->lock);

    page_table_t* pml4 = ctx->pml4;
    if (!pml4) {
        debug_printf("[VMM]   PML4: NULL\n");
        spin_unlock(&ctx->lock);
        return;
    }

    pte_t pml4_entry = pml4->entries[VMM_PML4_INDEX(virt_addr)];
    debug_printf("[VMM]   PML4 entry: 0x%016llx (present: %s)\n",
           (unsigned long long)pml4_entry, (pml4_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pml4_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[VMM_PDPT_INDEX(virt_addr)];
    debug_printf("[VMM]   PDPT entry: 0x%016llx (present: %s)\n",
           (unsigned long long)pdpt_entry, (pdpt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pdpt_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[VMM_PD_INDEX(virt_addr)];
    debug_printf("[VMM]   PD entry:   0x%016llx (present: %s)\n",
           (unsigned long long)pd_entry, (pd_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pd_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    // If PD entry is large page, no PT
    if (pd_entry & VMM_FLAG_LARGE_PAGE) {
        debug_printf("[VMM]   PD entry is a large page (2MB). Physical: 0x%p\n", (void*)vmm_pte_to_phys(pd_entry));
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    pte_t pt_entry = pt->entries[VMM_PT_INDEX(virt_addr)];
    debug_printf("[VMM]   PT entry:   0x%016llx (present: %s)\n",
           (unsigned long long)pt_entry, (pt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (pt_entry & VMM_FLAG_PRESENT) {
        uintptr_t phys_addr = vmm_pte_to_phys(pt_entry);
        debug_printf("[VMM]   -> Physical: 0x%p\n", (void*)phys_addr);
        debug_printf("[VMM]   -> Flags: %s%s%s%s\n",
               (pt_entry & VMM_FLAG_WRITABLE) ? "W" : "R",
               (pt_entry & VMM_FLAG_USER) ? "U" : "K",
               (pt_entry & VMM_FLAG_NO_EXECUTE) ? "NX" : "X",
               (pt_entry & VMM_FLAG_GLOBAL) ? "G" : "");
    }

    spin_unlock(&ctx->lock);
}

void vmm_dump_context_stats(vmm_context_t* ctx) {
    if (!ctx) return;

    spin_lock(&ctx->lock);

    debug_printf("[VMM] Context statistics:\n");
    debug_printf("[VMM]   PML4 physical:   0x%p\n", (void*)ctx->pml4_phys);
    debug_printf("[VMM]   Total pages:     %zu\n", ctx->mapped_pages);
    debug_printf("[VMM]   Kernel pages:    %zu\n", ctx->kernel_pages);
    debug_printf("[VMM]   User pages:      %zu\n", ctx->user_pages);
    debug_printf("[VMM]   Heap start:      0x%p\n", (void*)ctx->heap_start);
    debug_printf("[VMM]   Heap end:        0x%p\n", (void*)ctx->heap_end);
    debug_printf("[VMM]   Stack top:       0x%p\n", (void*)ctx->stack_top);

    spin_unlock(&ctx->lock);
}

void vmm_get_global_stats(vmm_stats_t* stats) {
    if (!stats) return;

    spin_lock(&vmm_global_lock);
    *stats = global_stats;
    spin_unlock(&vmm_global_lock);
}

void vmm_print_stats(void) {
    vmm_stats_t stats;
    vmm_get_global_stats(&stats);

    debug_printf("[VMM] Global statistics:\n");
    debug_printf("[VMM]   Total contexts:        %zu\n", stats.total_contexts);
    debug_printf("[VMM]   Total mapped pages:    %zu (%zu MB)\n",
           stats.total_mapped_pages,
           (stats.total_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   Kernel mapped pages:   %zu (%zu MB)\n",
           stats.kernel_mapped_pages,
           (stats.kernel_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   User mapped pages:     %zu (%zu MB)\n",
           stats.user_mapped_pages,
           (stats.user_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   Page tables allocated: %zu (%zu KB)\n",
           stats.page_tables_allocated,
           (stats.page_tables_allocated * VMM_PAGE_SIZE) / 1024);
    debug_printf("[VMM]   Page faults handled:   %zu\n", stats.page_faults_handled);
    debug_printf("[VMM]   TLB flushes:           %zu\n", stats.tlb_flushes);
}

// ========== BASIC TESTING ==========
void vmm_test_basic(void) {
    debug_printf("[VMM] %[H]Running basic VMM tests...%[D]\n");

    // Test 1: Create and destroy contexts
    debug_printf("[VMM] Test 1: Context creation/destruction...\n");
    vmm_context_t* test_ctx = vmm_create_context();
    if (!test_ctx) {
        debug_printf("[VMM] %[E]FAILED: Could not create context%[D]\n");
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Context created successfully%[D]\n");

    // Test 2: Map a page
    debug_printf("[VMM] Test 2: Page mapping...\n");
    void* phys_page = pmm_alloc(1);
    if (!phys_page) {
        debug_printf("[VMM] %[E]FAILED: Could not allocate physical page%[D]\n");
        vmm_destroy_context(test_ctx);
        return;
    }

    uintptr_t test_virt = 0x1000000; // 16MB
    vmm_map_result_t result = vmm_map_page(test_ctx, test_virt, (uintptr_t)phys_page,
                                          VMM_FLAGS_KERNEL_RW);
    if (!result.success) {
        debug_printf("[VMM] %[E]FAILED: Could not map page: %s%[D]\n", result.error_msg);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Page mapped successfully%[D]\n");

    // Test 3: Address translation
    debug_printf("[VMM] Test 3: Address translation...\n");
    uintptr_t translated = vmm_virt_to_phys(test_ctx, test_virt);
    if (translated != (uintptr_t)phys_page) {
        debug_printf("[VMM] %[E]FAILED: Translation mismatch (got 0x%p, expected 0x%p)%[D]\n",
               (void*)translated, phys_page);
        vmm_unmap_page(test_ctx, test_virt);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Address translation correct%[D]\n");

    // Test 4: Unmap page
    debug_printf("[VMM] Test 4: Page unmapping...\n");
    if (!vmm_unmap_page(test_ctx, test_virt)) {
        debug_printf("[VMM] %[E]FAILED: Could not unmap page%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }

    // Verify it's unmapped
    if (vmm_is_mapped(test_ctx, test_virt)) {
        debug_printf("[VMM] %[E]FAILED: Page still mapped after unmap%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Page unmapped successfully%[D]\n");

    // Cleanup
    pmm_free(phys_page, 1);
    vmm_destroy_context(test_ctx);

    // Test 5: Kernel heap allocation
    debug_printf("[VMM] Test 5: Kernel heap allocation...\n");

    void* heap_ptr = vmalloc(8192); // 2 pages
    
    if (!heap_ptr) {
        debug_printf("[VMM] %[E]FAILED: vmalloc failed%[D]\n");
        return;
    }

    // Try to write to it
    memset(heap_ptr, 0xAA, 8192);
    if (((uint8_t*)heap_ptr)[0] != 0xAA || ((uint8_t*)heap_ptr)[8191] != 0xAA) {
        debug_printf("[VMM] %[E]FAILED: Could not write to allocated memory%[D]\n");
        vfree(heap_ptr);
        return;
    }

    debug_printf("[VMM] %[S]PASSED: Kernel heap allocation works%[D]\n");

    vfree(heap_ptr);

    debug_printf("[VMM] %[S]All basic tests PASSED!%[D]\n");

    // Print statistics
    vmm_print_stats();
    vmm_dump_context_stats(kernel_context);
}
// ============================================================================
// PAGE FAULT HANDLING
// ============================================================================

// ============================================================================
// CABIN MEMORY MODEL SUPPORT
// ============================================================================
// Cabin: Isolated virtual address space with fixed layout
// 0x0000-0x0FFF: NULL trap (unmapped)
// 0x1000: Notify Page (User write, Kernel read)
// 0x2000: Result Page (User read, Kernel write)
// 0x3000+: Code/Data/Heap/Stack

vmm_context_t* vmm_create_cabin(uint64_t* notify_page_phys, uint64_t* result_page_phys) {
    if (!notify_page_phys || !result_page_phys) {
        vmm_set_error("Invalid output parameters for Cabin creation");
        return NULL;
    }

    // Create new context (address space)
    vmm_context_t* cabin_ctx = vmm_create_context();
    if (!cabin_ctx) {
        vmm_set_error("Failed to create Cabin context");
        return NULL;
    }

    // Allocate physical pages for Notify and Result
    void* notify_phys = pmm_alloc(1);
    if (!notify_phys) {
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate Notify page");
        return NULL;
    }

    void* result_phys = pmm_alloc(1);
    if (!result_phys) {
        pmm_free(notify_phys, 1);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate Result page");
        return NULL;
    }

    // Zero the allocated pages to ensure clean state
    memset(vmm_phys_to_virt((uintptr_t)notify_phys), 0, 4096);
    memset(vmm_phys_to_virt((uintptr_t)result_phys), 0, 4096);

    // Setup NULL trap (0x0000-0x0FFF unmapped)
    if (vmm_setup_null_trap(cabin_ctx) != 0) {
        pmm_free(result_phys, 1);
        pmm_free(notify_phys, 1);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to setup NULL trap");
        return NULL;
    }

    // Map Notify Page at 0x1000
    if (vmm_map_notify_page(cabin_ctx, (uintptr_t)notify_phys) != 0) {
        pmm_free(result_phys, 1);
        pmm_free(notify_phys, 1);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map Notify page");
        return NULL;
    }

    // Map Result Page at 0x2000
    if (vmm_map_result_page(cabin_ctx, (uintptr_t)result_phys) != 0) {
        pmm_free(result_phys, 1);
        pmm_free(notify_phys, 1);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map Result page");
        return NULL;
    }

    // Map CPU capabilities page at 0x7FFFF000 (read-only, before stack)
    if (g_cpu_caps_page_phys != 0) {
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;  // Read-only for userspace
        vmm_map_result_t result = vmm_map_page(cabin_ctx, BOX_CPU_CAPS_PAGE_ADDR, g_cpu_caps_page_phys, flags);
        if (!result.success) {
            debug_printf("[VMM] WARNING: Failed to map CPU caps page at 0x%lx: %s\n",
                         BOX_CPU_CAPS_PAGE_ADDR, result.error_msg);
            // Non-fatal error - continue without CPU caps page
        }
    }

    // Output physical addresses for kernel access
    *notify_page_phys = (uint64_t)notify_phys;
    *result_page_phys = (uint64_t)result_phys;

    // Return context (caller can get PML4 phys via cabin_ctx->pml4_phys for CR3)
    return cabin_ctx;
}

int vmm_map_notify_page(vmm_context_t* ctx, uintptr_t phys_page) {
    if (!ctx) return -1;

    // Notify Page: User write, Kernel read
    // User can write to notify kernel, kernel can read notifications
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_page(ctx, VMM_CABIN_NOTIFY_PAGE, phys_page, flags);
    if (!result.success) {
        debug_printf("[VMM] Failed to map Notify Page: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

int vmm_map_result_page(vmm_context_t* ctx, uintptr_t phys_page) {
    if (!ctx) return -1;

    // Result Page: User read-write, Kernel read-write
    // User needs to update head index in ResultRing after reading results
    // Kernel writes results, user reads and updates head pointer
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_page(ctx, VMM_CABIN_RESULT_PAGE, phys_page, flags);
    if (!result.success) {
        debug_printf("[VMM] Failed to map Result Page: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

int vmm_setup_null_trap(vmm_context_t* ctx) {
    if (!ctx) return -1;

    // NULL trap zone (0x0000-0x0FFF) is left unmapped
    // Any access will trigger page fault
    // We don't need to explicitly map it - just ensure it's NOT mapped
    // This is already the default state after vmm_create_context()

    return 0;  // Success (nothing to do, already unmapped)
}

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD    1
#define PF_X       1
#define PF_W       2
#define PF_R       4

int vmm_map_code_region(vmm_context_t* ctx, uintptr_t code_phys, uint64_t size) {
    if (!ctx || !code_phys || size == 0) return -1;

    void* elf_virt = vmm_phys_to_virt(code_phys);
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_virt;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        // Raw flat binary — map directly at VMM_CABIN_CODE_START
        debug_printf("[VMM] No ELF magic, loading as raw flat binary at 0x%llx\n",
                     (uint64_t)VMM_CABIN_CODE_START);
        uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
        if (pages < 1) pages = 1;
        vmm_map_result_t raw_result = vmm_map_pages(ctx, VMM_CABIN_CODE_START,
                                                    code_phys, (size_t)pages,
                                                    VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        if (!raw_result.success) {
            debug_printf("[VMM] ERROR: Failed to map raw binary: %s\n", raw_result.error_msg);
            return -1;
        }
        debug_printf("[VMM] Raw binary mapped: %llu pages at virt=0x%llx phys=0x%lx\n",
                     pages, (uint64_t)VMM_CABIN_CODE_START, code_phys);
        return 0;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        debug_printf("[VMM] ERROR: No program headers found\n");
        return -1;
    }

    debug_printf("[VMM] Parsing ELF: %u program headers at offset 0x%llx\n",
                 ehdr->e_phnum, ehdr->e_phoff);

    Elf64_Phdr* phdr_base = (Elf64_Phdr*)((uint8_t*)elf_virt + ehdr->e_phoff);
    size_t total_mapped_pages = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = &phdr_base[i];

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t file_offset = phdr->p_offset;
        uint32_t flags = phdr->p_flags;

        if (memsz == 0) {
            continue;
        }

        uint64_t vaddr_aligned = vaddr & VMM_PAGE_MASK;
        uint64_t page_offset = vaddr & VMM_PAGE_OFFSET_MASK;
        uint64_t memsz_aligned = ((memsz + page_offset + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE) * VMM_PAGE_SIZE;
        size_t page_count = memsz_aligned / VMM_PAGE_SIZE;

        if (vaddr_aligned < 0x3000) {
            debug_printf("[VMM] ERROR: Invalid ELF segment vaddr 0x%llx (below 0x3000)\n", vaddr_aligned);
            return -BOXOS_ERR_INVALID_ARGUMENT;
        }

        void* segment_phys_ptr = pmm_alloc(page_count);
        if (!segment_phys_ptr) {
            debug_printf("[VMM] ERROR: Failed to allocate %zu pages for segment %u\n", page_count, i);
            return -1;
        }
        uintptr_t segment_phys = (uintptr_t)segment_phys_ptr;

        void* segment_virt = vmm_phys_to_virt(segment_phys);
        memset(segment_virt, 0, memsz_aligned);

        if (filesz > 0) {
            void* file_data = (uint8_t*)elf_virt + file_offset;
            memcpy((uint8_t*)segment_virt + page_offset, file_data, filesz);
        }

        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

        if (flags & PF_W) {
            vmm_flags |= VMM_FLAG_WRITABLE;
        }

        if (!(flags & PF_X)) {
            vmm_flags |= VMM_FLAG_NO_EXECUTE;
        }

        vmm_map_result_t result = vmm_map_pages(ctx, vaddr_aligned, segment_phys,
                                                page_count, vmm_flags);
        if (!result.success) {
            debug_printf("[VMM] ERROR: Failed to map segment %u: %s\n", i, result.error_msg);
            pmm_free((void*)segment_phys, page_count);
            return -1;
        }

        const char* perm_str = "";
        if ((flags & PF_R) && (flags & PF_W) && !(flags & PF_X)) {
            perm_str = "R+W, NX";
        } else if ((flags & PF_R) && (flags & PF_X) && !(flags & PF_W)) {
            perm_str = "R+X";
        } else if ((flags & PF_R) && !(flags & PF_W) && !(flags & PF_X)) {
            perm_str = "R, NX";
        } else {
            perm_str = "custom";
        }

        debug_printf("[VMM] Mapped segment %u: virt=0x%04llx-0x%04llx phys=0x%lx pages=%zu flags=%s filesz=%llu memsz=%llu\n",
                     i, vaddr_aligned, vaddr_aligned + memsz_aligned - 1,
                     segment_phys, page_count, perm_str, filesz, memsz);

        total_mapped_pages += page_count;
    }

    debug_printf("[VMM] ELF binary mapped: %zu total pages (W^X enforced)\n", total_mapped_pages);

    return 0;
}

// ============================================================================
// PAGE FAULT HANDLING
// ============================================================================

// Page fault error code bits
#define PF_PRESENT   (1 << 0)  // 0 = not present, 1 = protection fault
#define PF_WRITE     (1 << 1)  // 0 = read, 1 = write
#define PF_USER      (1 << 2)  // 0 = kernel, 1 = user mode
#define PF_RESERVED  (1 << 3)  // 1 = reserved bit set in page table
#define PF_INSTR     (1 << 4)  // 1 = instruction fetch

int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code) {
    // Analyze fault
    bool present = error_code & PF_PRESENT;
    bool write = error_code & PF_WRITE;
    bool user = error_code & PF_USER;
    bool reserved = error_code & PF_RESERVED;
    bool instr_fetch = error_code & PF_INSTR;

    debug_printf("[VMM] Page fault at 0x%llx (error=0x%llx)\n", fault_addr, error_code);
    debug_printf("[VMM]   present=%d write=%d user=%d reserved=%d instr=%d\n",
            present, write, user, reserved, instr_fetch);

    // Reserved bit violations are always fatal
    if (reserved) {
        debug_printf("[VMM] ERROR: Reserved bit violation - cannot handle\n");
        return -1;
    }

    // Check for kernel stack overflow (guard page access)
    process_t* current = process_get_current();
    if (current && current->kernel_stack_guard_base) {
        uintptr_t guard_start = (uintptr_t)current->kernel_stack_guard_base;
        uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

        if (fault_addr >= guard_start && fault_addr < guard_end) {
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM] FATAL: Kernel stack overflow detected!\n");
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM]   Process PID: %u\n", current->pid);
            debug_printf("[VMM]   Fault address: 0x%016lx\n", fault_addr);
            debug_printf("[VMM]   Guard region: 0x%016lx - 0x%016lx\n", guard_start, guard_end);
            debug_printf("[VMM]   Stack region: 0x%016lx - 0x%016lx\n",
                         (uintptr_t)current->kernel_stack,
                         (uintptr_t)current->kernel_stack_top);
            debug_printf("[VMM]   Error code: 0x%llx\n", error_code);
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM] System will halt to prevent corruption\n");
            return -1;
        }
    }

    // Check if fault is in user stack guard page range
    vmm_context_t* ctx = vmm_get_current_context();
    if (ctx && user) {
        // Calculate guard page range for current process
        uint64_t stack_top = VMM_USER_STACK_TOP;
        uint64_t guard_base = stack_top - (CONFIG_USER_STACK_TOTAL_PAGES * VMM_PAGE_SIZE);
        uint64_t guard_end = guard_base + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

        if (fault_addr >= guard_base && fault_addr < guard_end) {
            debug_printf("[VMM] ERROR: Stack overflow detected (guard page access at 0x%llx)\n", fault_addr);
            return -1;  // Signal unrecoverable error -> kill process
        }
    }

    // If page is present, it's a protection fault
    if (present) {
        debug_printf("[VMM] ERROR: Protection fault - access denied\n");
        return -1;
    }

    // Page not present - demand paging
    debug_printf("[VMM] Page not present - attempting demand paging\n");

    // Reuse ctx from guard page check above (or get current context)
    if (!ctx) {
        ctx = kernel_context;
    }

    // Align fault address to page boundary
    uintptr_t page_addr = fault_addr & ~(VMM_PAGE_SIZE - 1);

    // Check if this is in kernel heap range (higher half)
    if (page_addr >= VMM_KERNEL_HEAP_BASE &&
        page_addr < VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE) {

        debug_printf("[VMM] Demand paging: mapping kernel heap page at 0x%llx\n", page_addr);

        // Allocate physical page
        void* phys_page = pmm_alloc(1);
        if (!phys_page) {
            debug_printf("[VMM] ERROR: Failed to allocate physical page\n");
            return -1;
        }

        // Map the page
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        if (user) {
            flags |= VMM_FLAG_USER;
        }

        vmm_map_result_t result = vmm_map_page(ctx, page_addr, (uintptr_t)phys_page, flags);
        if (!result.success) {
            debug_printf("[VMM] ERROR: Failed to map page\n");
            pmm_free(phys_page, 1);
            return -1;
        }

        debug_printf("[VMM] SUCCESS: Demand paging successful\n");
        return 0;  // Handled successfully
    }

    // Check if this is in low memory (0-256MB) — restore identity mapping
    if (page_addr < (256ULL * 1024 * 1024)) {
        debug_printf("[VMM] WARNING: Restoring identity mapping for 0x%llx\n", page_addr);
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        vmm_map_result_t result = vmm_map_page(kernel_context, page_addr, page_addr, flags);
        if (!result.success) {
            debug_printf("[VMM] FATAL: Failed to restore identity mapping at 0x%llx\n", page_addr);
            return -1;
        }
        return 0;
    }

    // Not in a valid range
    debug_printf("[VMM] ERROR: Fault address not in valid range (0x%llx)\n", fault_addr);
    return -1;  // Cannot handle
}
