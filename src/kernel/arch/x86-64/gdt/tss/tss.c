#include "tss.h"
#include "gdt.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"

static tss_t kernel_tss;

// Phase 1: static stacks for early boot (before PMM/VMM)
static uint8_t ist_stacks_boot[IST_COUNT][IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t kernel_stack_boot[32768] __attribute__((aligned(16)));

// Phase 2: dynamic IST stacks with guard pages (set by tss_setup_dynamic_stacks)
static void *ist_guard_bases[IST_COUNT];  // for diagnostics

void tss_init(void) {
    debug_printf("[TSS] Initializing Task State Segment...\n");

    memset(&kernel_tss, 0, sizeof(tss_t));

    kernel_tss.rsp0 = (uint64_t)kernel_stack_boot + sizeof(kernel_stack_boot) - 16;
    kernel_tss.rsp1 = 0;
    kernel_tss.rsp2 = 0;

    // IST1-5 only, IST6-7 unused
    for (int i = 0; i < 5; i++) {
        uint64_t stack_top = (uint64_t)ist_stacks_boot[i] + IST_STACK_SIZE - 16;

        switch(i + 1) {
            case IST_DOUBLE_FAULT:
                kernel_tss.ist1 = stack_top;
                debug_printf("[TSS] IST1 (Double Fault): 0x%p\n", (void*)stack_top);
                break;
            case IST_NMI:
                kernel_tss.ist2 = stack_top;
                debug_printf("[TSS] IST2 (NMI): 0x%p\n", (void*)stack_top);
                break;
            case IST_MACHINE_CHECK:
                kernel_tss.ist3 = stack_top;
                debug_printf("[TSS] IST3 (Machine Check): 0x%p\n", (void*)stack_top);
                break;
            case IST_DEBUG:
                kernel_tss.ist4 = stack_top;
                debug_printf("[TSS] IST4 (Debug): 0x%p\n", (void*)stack_top);
                break;
            case IST_STACK_FAULT:
                kernel_tss.ist5 = stack_top;
                debug_printf("[TSS] IST5 (Stack Fault): 0x%p\n", (void*)stack_top);
                break;
        }
    }

    kernel_tss.iomap_base = sizeof(tss_t);  // No I/O bitmap

    debug_printf("[TSS] RSP0 (Ring 0 stack): 0x%p\n", (void*)kernel_tss.rsp0);
    debug_printf("[TSS] IOMAP base: 0x%04x\n", kernel_tss.iomap_base);

    gdt_set_tss_entry(5, (uint64_t)&kernel_tss, sizeof(tss_t) - 1);

    debug_printf("[TSS] TSS configured at 0x%p (size: %d bytes)\n",
           (void*)&kernel_tss, sizeof(tss_t));

    tss_load();

    debug_printf("[TSS] %[S]TSS loaded successfully!%[D]\n");
}

void tss_setup_dynamic_stacks(void) {
    debug_printf("[TSS] Setting up dynamic IST stacks with guard pages...\n");

    vmm_context_t *kernel_ctx = vmm_get_kernel_context();
    const char *ist_names[] = {"Double Fault", "NMI", "Machine Check", "Debug", "Stack Fault"};
    for (int i = 0; i < IST_COUNT; i++) {
        size_t total_pages = IST_GUARD_PAGES + IST_STACK_PAGES;
        void *phys = pmm_alloc(total_pages);
        if (!phys) {
            kprintf("[TSS] FATAL: Failed to allocate IST%d stack (%s)\n",
                    i + 1, ist_names[i]);
            while (1) { asm volatile("cli; hlt"); }
        }

        void *virt_base = vmm_phys_to_virt((uintptr_t)phys);
        ist_guard_bases[i] = virt_base;

        // Unmap guard page (first page)
        pte_t *guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)virt_base);
        if (guard_pte) {
            *guard_pte = 0;
            vmm_flush_tlb_page((uintptr_t)virt_base);
        }

        // Stack top = base + guard + data - 16 (alignment)
        uint64_t stack_top = (uint64_t)virt_base + (total_pages * 4096) - 16;

        switch (i) {
            case 0: kernel_tss.ist1 = stack_top; break;
            case 1: kernel_tss.ist2 = stack_top; break;
            case 2: kernel_tss.ist3 = stack_top; break;
            case 3: kernel_tss.ist4 = stack_top; break;
            case 4: kernel_tss.ist5 = stack_top; break;
        }

        debug_printf("[TSS] IST%d (%s): guard=0x%p stack_top=0x%p [dynamic]\n",
                     i + 1, ist_names[i], virt_base, (void *)stack_top);
    }

    debug_printf("[TSS] %[S]Dynamic IST stacks with guard pages ready%[D]\n");
}

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

void tss_load(void) {
    debug_printf("[TSS] Loading TSS (selector 0x%02x)...\n", GDT_TSS);

    asm volatile("ltr %0" : : "r" ((uint16_t)GDT_TSS));
}

tss_t* tss_get_ptr(void) {
    return &kernel_tss;
}

uint64_t tss_get_ist_stack(int ist_num) {
    // access IST entries by index without pointer arithmetic on struct members
    switch (ist_num) {
        case 1: return kernel_tss.ist1;
        case 2: return kernel_tss.ist2;
        case 3: return kernel_tss.ist3;
        case 4: return kernel_tss.ist4;
        case 5: return kernel_tss.ist5;
        case 6: return kernel_tss.ist6;
        case 7: return kernel_tss.ist7;
        default: return 0;
    }
}
