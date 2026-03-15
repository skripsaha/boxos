#include "amp.h"
#include "lapic.h"
#include "gdt.h"
#include "idt.h"
#include "klib.h"
#include "fpu.h"

void ap_entry_c(uint64_t core_index) {
    gdt_load();

    idt_load();

    enable_fpu();

    // Enable LAPIC: set spurious vector register with enable bit.
    // lapic_base_virt is already mapped by BSP and visible via shared page tables.
    lapic_enable();

    // EFER.NXE is per-core — set it explicitly
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1 << 11);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080), "a"(lo), "d"(hi));

    g_amp.cores[core_index].online = true;

    kprintf("[AMP] Core %u online (LAPIC ID %u)\n",
            (uint32_t)core_index, lapic_get_id());

    __asm__ volatile("sti");
    while (1) {
        __asm__ volatile("hlt");
    }
}
