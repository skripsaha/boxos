#include "tss.h"
#include "gdt.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"

// Глобальный TSS
static tss_t kernel_tss;

// IST стеки (выделяем статически для простоты)
static uint8_t ist_stacks[4][IST_STACK_SIZE] __attribute__((aligned(16)));

// Основной kernel стек для ring 0 (32KB для глубокого call stack)
static uint8_t kernel_stack[32768] __attribute__((aligned(16)));

// Внешние функции из gdt.c
extern void gdt_set_tss_entry(int index, uint64_t base, uint64_t limit);

void tss_init(void) {
    debug_printf("[TSS] Initializing Task State Segment...\n");

    // Очищаем TSS
    memset(&kernel_tss, 0, sizeof(tss_t));

    // Настройка основных стеков
    kernel_tss.rsp0 = (uint64_t)kernel_stack + sizeof(kernel_stack) - 16;  // Ring 0 stack
    kernel_tss.rsp1 = 0;  // Ring 1 не используется в 64-bit
    kernel_tss.rsp2 = 0;  // Ring 2 не используется в 64-bit
    
    // Настройка IST стеков (IST1-4 only, IST5-7 unused)
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
    
    // IOPL bitmap - ставим в конец TSS (нет I/O bitmap)
    kernel_tss.iomap_base = sizeof(tss_t);
    
    debug_printf("[TSS] RSP0 (Ring 0 stack): 0x%p\n", (void*)kernel_tss.rsp0);
    debug_printf("[TSS] IOMAP base: 0x%04x\n", kernel_tss.iomap_base);
    
    // Добавляем TSS в GDT (обновляем существующую запись)
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
    
    // Загружаем TSS через LTR инструкцию
    asm volatile("ltr %0" : : "r" ((uint16_t)GDT_TSS));
}

void tss_test(void) {
    debug_printf("[TSS] %[H]Testing TSS...%[D]\n");
    
    // Проверяем, что TSS загружен
    uint16_t current_tr;
    asm volatile("str %0" : "=r" (current_tr));
    
    debug_printf("[TSS] Current TR (Task Register): 0x%04x (expected: 0x%04x)\n", 
           current_tr, GDT_TSS);
    
    if (current_tr == GDT_TSS) {
        debug_printf("[TSS] %[S]TSS load verification: PASSED%[D]\n");
    } else {
        debug_printf("[TSS] %[E]TSS load verification: FAILED%[D]\n");
        return;
    }
    
    // Проверяем настройки TSS
    debug_printf("[TSS] TSS configuration check:\n");
    debug_printf("[TSS]   RSP0: 0x%p\n", (void*)kernel_tss.rsp0);
    debug_printf("[TSS]   IST1 (Double Fault): 0x%p\n", (void*)kernel_tss.ist1);
    debug_printf("[TSS]   IST2 (NMI): 0x%p\n", (void*)kernel_tss.ist2);
    debug_printf("[TSS]   IST3 (Machine Check): 0x%p\n", (void*)kernel_tss.ist3);
    debug_printf("[TSS]   IST4 (Debug): 0x%p\n", (void*)kernel_tss.ist4);
    
    // Проверяем, что стеки IST не пересекаются
    bool stack_overlap = false;
    for (int i = 0; i < 4; i++) {
        uint64_t stack_a = *(&kernel_tss.ist1 + i);
        for (int j = i + 1; j < 4; j++) {
            uint64_t stack_b = *(&kernel_tss.ist1 + j);
            if (stack_a != 0 && stack_b != 0) {
                uint64_t diff = (stack_a > stack_b) ? (stack_a - stack_b) : (stack_b - stack_a);
                if (diff < IST_STACK_SIZE) {
                    debug_printf("[TSS] %[E]WARNING: IST%d and IST%d stacks may overlap!%[D]\n", 
                           i + 1, j + 1);
                    stack_overlap = true;
                }
            }
        }
    }
    
    if (!stack_overlap) {
        debug_printf("[TSS] %[S]IST stack layout: OK%[D]\n");
    }
    
    debug_printf("[TSS] %[S]TSS test PASSED!%[D]\n");
    debug_printf("[TSS] %[W]Note: IST functionality will be tested when exceptions with IST occur%[D]\n");
}

uint64_t tss_get_ist_stack(int ist_num) {
    if (ist_num < 1 || ist_num > 7) {
        return 0;
    }
    
    return *(&kernel_tss.ist1 + ist_num - 1);
}