#include "gdt.h"
#include "klib.h"
#include "tss.h"

// GDT table (7 entries: null, kernel code, kernel data, user code, user data, TSS low, TSS high)
static gdt_entry_t gdt[7];
static gdt_descriptor_t gdt_desc;

static void gdt_load_asm(uint64_t gdt_desc_addr) {
    /*
     * Reload CS via LJMP through memory (`ljmpq *m16:64`), NOT LRETQ.
     *
     * The classic `push CS; push label; lretq` pattern is forbidden under
     * IA32_S_CET.SH_STK_EN=1: LRETQ pops CS+RIP from the shadow stack
     * and #CPs when no matching FAR CALL pushed there. LJMP through a
     * memory operand performs the same CS:RIP transfer without touching
     * the shadow stack (CALL/RET semantics belong to near returns and
     * far interrupt-frame returns; LJMP is treated as a tagged JMP).
     *
     * Layout of the 10-byte m16:64 operand on the stack:
     *   [rsp + 0]  64-bit target RIP (offset)
     *   [rsp + 8]  16-bit selector  (CS)
     * 16-byte slot keeps RSP aligned and matches Intel SDM Vol 2
     * Table 2-1 ModR/M decoding for `ljmp *m16:64`.
     */
    asm volatile (
        "lgdt (%0)\n\t"
        "subq $16, %%rsp\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, (%%rsp)\n\t"
        "movw %w1, 8(%%rsp)\n\t"
        /* REX.W + FF /5 ModR/M=2C SIB=24 — `ljmpq *(%rsp)`. Older
         * binutils don't accept `ljmpq` mnemonic for m16:64; raw bytes
         * are portable across GAS versions. */
        ".byte 0x48, 0xff, 0x2c, 0x24\n\t"
        "1:\n\t"
        "addq $16, %%rsp\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss"
        :
        : "r" (gdt_desc_addr),
          "i" ((uint64_t)GDT_KERNEL_CODE),
          "i" ((uint32_t)GDT_KERNEL_DATA)
        : "memory", "rax"
    );
}

void gdt_set_entry(int index, uint64_t base, uint64_t limit, uint8_t access, uint8_t flags) {
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].access = access;
    gdt[index].granularity = (flags & 0xF0) | ((limit >> 16) & 0x0F);
    gdt[index].base_high = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    debug_printf("[GDT] Initializing Global Descriptor Table...\n");

    memset(gdt, 0, sizeof(gdt));

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);  // User Data — index 3 (SYSRET: SS = STAR+8)
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);  // User Code — index 4 (SYSRET: CS = STAR+16)

    // Indices 5 and 6 reserved for TSS (16 bytes in x86-64)
    gdt_desc.limit = sizeof(gdt) - 1;
    gdt_desc.base = (uint64_t)gdt;

    debug_printf("[GDT] GDT entries configured:\n");
    kprintf("  - Kernel Code: 0x%02X\n", GDT_KERNEL_CODE);
    kprintf("  - Kernel Data: 0x%02X\n", GDT_KERNEL_DATA);
    kprintf("  - User Code: 0x%02X\n", GDT_USER_CODE);
    kprintf("  - User Data: 0x%02X\n", GDT_USER_DATA);
    kprintf("  - TSS: 0x%02X (will be set by tss_init)\n", GDT_TSS);

    gdt_load();

    debug_printf("[GDT] %[S]GDT loaded successfully!%[D]\n");
}

void gdt_set_tss_entry(int index, uint64_t base, uint64_t limit) {
    // TSS descriptor occupies 2 GDT entries (16 bytes) in x86-64
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].access = 0x89;  // Present=1, DPL=0, Available TSS (not busy)
    gdt[index].granularity = ((limit >> 16) & 0x0F);
    gdt[index].base_high = (base >> 24) & 0xFF;

    // Second entry holds upper 32 bits of base address
    uint64_t* second_entry = (uint64_t*)&gdt[index + 1];
    *second_entry = (base >> 32);

    debug_printf("[GDT] TSS entry set: index=%d-%d, base=0x%p, limit=0x%llx\n",
           index, index + 1, (void*)base, limit);
    debug_printf("[GDT] First entry:  0x%016llx\n", *(uint64_t*)&gdt[index]);
    debug_printf("[GDT] Second entry: 0x%016llx\n", *second_entry);
}

void gdt_load(void) {
    debug_printf("[GDT] Loading GDT at 0x%p (limit: %d)...\n", (void*)gdt_desc.base, gdt_desc.limit);
    gdt_load_asm((uint64_t)&gdt_desc);
}

void gdt_copy_entries(gdt_entry_t* dest, int count) {
    if (count > 7) count = 7;
    memcpy(dest, gdt, sizeof(gdt_entry_t) * count);
}
