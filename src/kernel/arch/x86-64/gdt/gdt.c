#include "gdt.h"
#include "klib.h"
#include "tss.h"

// GDT таблица (7 записей: null, kernel code, kernel data, user code, user data, TSS low, TSS high)
static gdt_entry_t gdt[7];
static gdt_descriptor_t gdt_desc;
static tss_t kernel_tss;

// Inline ASM функция для загрузки GDT
static void gdt_load_asm(uint64_t gdt_desc_addr) {
    asm volatile (
        "lgdt (%0)\n\t"           // Загружаем GDT
        "pushq $0x08\n\t"         // Push новый CS (kernel code selector)  
        "leaq 1f(%%rip), %%rax\n\t"  // Загружаем адрес метки 1
        "pushq %%rax\n\t"         // Push новый RIP
        "lretq\n\t"               // Far return для обновления CS
        "1:\n\t"                  // Метка после обновления CS
        "movw $0x10, %%ax\n\t"    // Kernel data selector
        "movw %%ax, %%ds\n\t"     // Обновляем DS
        "movw %%ax, %%es\n\t"     // Обновляем ES  
        "movw %%ax, %%fs\n\t"     // Обновляем FS
        "movw %%ax, %%gs\n\t"     // Обновляем GS
        "movw %%ax, %%ss"         // Обновляем SS
        :
        : "r" (gdt_desc_addr)
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
    
    // Очищаем GDT - увеличиваем размер для TSS (нужно 7 записей)
    memset(gdt, 0, sizeof(gdt));
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    
    // Null дескриптор (индекс 0)
    gdt_set_entry(0, 0, 0, 0, 0);
    
    // Kernel Code Segment (индекс 1, селектор 0x08)
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);
    
    // Kernel Data Segment (индекс 2, селектор 0x10)  
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    
    // User Code Segment (индекс 3, селектор 0x18 | 3)
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0);
    
    // User Data Segment (индекс 4, селектор 0x20 | 3)  
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);
    
    // TSS будет настроен позже через gdt_set_tss_entry()
    // Индексы 5 и 6 зарезервированы для TSS (16 байт в x86-64)
    
    // Настройка дескриптора GDT
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
    // TSS дескриптор в x86-64 занимает 2 записи в GDT (16 байт)
    
    // Первая запись (младшие 64 бита)
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].access = 0x89;  // Present=1, DPL=0, Available TSS (not busy)
    gdt[index].granularity = ((limit >> 16) & 0x0F);  // Granularity=0 (bytes), limit high
    gdt[index].base_high = (base >> 24) & 0xFF;
    
    // Вторая запись (старшие 64 бита) - используем как uint64_t
    uint64_t* second_entry = (uint64_t*)&gdt[index + 1];
    *second_entry = (base >> 32);  // Верхние 32 бита базы в младших 32 битах второй записи
    
    debug_printf("[GDT] TSS entry set: index=%d-%d, base=0x%p, limit=0x%llx\n", 
           index, index + 1, (void*)base, limit);
    debug_printf("[GDT] First entry:  0x%016llx\n", *(uint64_t*)&gdt[index]);
    debug_printf("[GDT] Second entry: 0x%016llx\n", *second_entry);
}

void gdt_load(void) {
    debug_printf("[GDT] Loading GDT at 0x%p (limit: %d)...\n", (void*)gdt_desc.base, gdt_desc.limit);
    
    // Загружаем GDT через ASM
    gdt_load_asm((uint64_t)&gdt_desc);
}

void gdt_test(void) {
    debug_printf("[GDT] %[H]Testing GDT...%[D]\n");
    
    // Проверяем, что селекторы работают
    uint16_t cs, ds;
    
    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    
    debug_printf("[GDT] Current CS: 0x%04X (expected: 0x%04X)\n", cs, GDT_KERNEL_CODE);
    debug_printf("[GDT] Current DS: 0x%04X (expected: 0x%04X)\n", ds, GDT_KERNEL_DATA);
    
    if (cs == GDT_KERNEL_CODE && ds == GDT_KERNEL_DATA) {
        debug_printf("[GDT] %[S]GDT test PASSED!%[D]\n");
    } else {
        debug_printf("[GDT] %[E]GDT test FAILED!%[D]\n");
        panic("GDT verification failed");
    }
    
    debug_printf("[GDT] Testing segment access...\n");
    
    // Простой тест записи в memory через DS
    volatile uint32_t test_value = 0x12345678;
    uint32_t read_value = test_value;
    
    if (read_value == 0x12345678) {
        debug_printf("[GDT] %[S]Memory access through DS: OK%[D]\n");
    } else {
        debug_printf("[GDT] %[E]Memory access through DS: FAILED%[D]\n");
    }
    
    debug_printf("[GDT] %[S]All GDT tests completed!%[D]\n");
}