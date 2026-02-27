#ifndef TSS_H
#define TSS_H

#include "ktypes.h"

// IST индексы для критических прерываний
#define IST_DOUBLE_FAULT    1
#define IST_NMI            2  
#define IST_MACHINE_CHECK  3
#define IST_DEBUG          4

// Размеры стеков IST
#define IST_STACK_SIZE     4096  // 4KB на каждый IST стек

// Полная структура TSS для x86-64
typedef struct {
    uint32_t reserved1;
    uint64_t rsp0;      // Stack pointer for ring 0
    uint64_t rsp1;      // Stack pointer for ring 1  
    uint64_t rsp2;      // Stack pointer for ring 2
    uint64_t reserved2;
    uint64_t ist1;      // IST #1 - Double Fault
    uint64_t ist2;      // IST #2 - NMI
    uint64_t ist3;      // IST #3 - Machine Check
    uint64_t ist4;      // IST #4 - Debug
    uint64_t ist5;      // IST #5 - не используется
    uint64_t ist6;      // IST #6 - не используется
    uint64_t ist7;      // IST #7 - не используется
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

// Дескриптор TSS в GDT (занимает 2 записи в x86-64)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;      // 0x89 для available TSS
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;  // Верхние 32 бита базы (только в x86-64)
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

// Функции
void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);
void tss_load(void);
void tss_test(void);

// Получение указателей на IST стеки
uint64_t tss_get_ist_stack(int ist_num);

#endif // TSS_H