#ifndef IDT_H
#define IDT_H

#include "ktypes.h"
#include "kernel_config.h"
#include "tss.h"
#include "boxos_limits.h"


#define IDT_TYPE_INTERRUPT_GATE 0x8E
#define IDT_TYPE_TRAP_GATE      0x8F
#define IDT_TYPE_USER_INTERRUPT 0xEE

#define SYSCALL_VECTOR          CONFIG_SYSCALL_VECTOR

#define EXCEPTION_DIVIDE_ERROR      0
#define EXCEPTION_DEBUG             1
#define EXCEPTION_NMI               2
#define EXCEPTION_BREAKPOINT        3
#define EXCEPTION_OVERFLOW          4
#define EXCEPTION_BOUND_RANGE       5
#define EXCEPTION_INVALID_OPCODE    6
#define EXCEPTION_DEVICE_NOT_AVAIL  7
#define EXCEPTION_DOUBLE_FAULT      8
#define EXCEPTION_INVALID_TSS       10
#define EXCEPTION_SEGMENT_NOT_PRESENT 11
#define EXCEPTION_STACK_FAULT       12
#define EXCEPTION_GENERAL_PROTECTION 13
#define EXCEPTION_PAGE_FAULT        14
#define EXCEPTION_FPU_ERROR         16
#define EXCEPTION_ALIGNMENT_CHECK   17
#define EXCEPTION_MACHINE_CHECK     18
#define EXCEPTION_SIMD_EXCEPTION    19

#define IRQ_TIMER       32
#define IRQ_KEYBOARD    33
#define IRQ_CASCADE     34
#define IRQ_COM2        35
#define IRQ_COM1        36
#define IRQ_LPT2        37
#define IRQ_FLOPPY      38
#define IRQ_LPT1        39
#define IRQ_RTC         40
#define IRQ_FREE1       41
#define IRQ_FREE2       42
#define IRQ_FREE3       43
#define IRQ_MOUSE       44
#define IRQ_FPU         45
#define IRQ_ATA_PRIMARY 46
#define IRQ_ATA_SECONDARY 47

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_descriptor_t;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) interrupt_frame_t;

typedef void (*irq_callback_t)(void);

void idt_init(void);
void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist);
void idt_load(void);
void idt_test(void);

void irq_register_handler(uint8_t irq, irq_callback_t callback);
void irq_unregister_handler(uint8_t irq);

void exception_handler(interrupt_frame_t* frame);
void irq_handler(interrupt_frame_t* frame);
void syscall_handler(interrupt_frame_t* frame);

void idt_set_syscall_mode(bool multicore);

extern void* isr_table[IDT_ENTRIES];

#endif