#include "tss.h"
#include "gdt.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"

static tss_t kernel_tss;
static uint8_t ist_stacks[4][IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t kernel_stack[32768] __attribute__((aligned(16)));

extern void gdt_set_tss_entry(int index, uint64_t base, uint64_t limit);

void tss_init(void) {
    debug_printf("[TSS] Initializing Task State Segment...\n");

    memset(&kernel_tss, 0, sizeof(tss_t));

    kernel_tss.rsp0 = (uint64_t)kernel_stack + sizeof(kernel_stack) - 16;
    kernel_tss.rsp1 = 0;
    kernel_tss.rsp2 = 0;

    // IST1-4 only, IST5-7 unused
    for (int i = 0; i < 4; i++) {
        uint64_t stack_top = (uint64_t)ist_stacks[i] + IST_STACK_SIZE - 16;

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

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

void tss_load(void) {
    debug_printf("[TSS] Loading TSS (selector 0x%02x)...\n", GDT_TSS);

    asm volatile("ltr %0" : : "r" ((uint16_t)GDT_TSS));
}

uint64_t tss_get_ist_stack(int ist_num) {
    if (ist_num < 1 || ist_num > 7) {
        return 0;
    }

    return *(&kernel_tss.ist1 + ist_num - 1);
}
