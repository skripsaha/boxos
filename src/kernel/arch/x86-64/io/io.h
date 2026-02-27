#ifndef IO_H
#define IO_H

#include "ktypes.h"

// ========== Port I/O ==========

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

// ========== MMIO ==========

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *((volatile uint32_t*)addr);
}

static inline void mmio_write32(uintptr_t addr, uint32_t value) {
    *((volatile uint32_t*)addr) = value;
}

static inline uint64_t mmio_read64(uintptr_t addr) {
    return *((volatile uint64_t*)addr);
}

static inline void mmio_write64(uintptr_t addr, uint64_t value) {
    *((volatile uint64_t*)addr) = value;
}

// ========== Interrupt Control ==========

static inline void cli(void) {
    __asm__ volatile ("cli");
}

static inline void sti(void) {
    __asm__ volatile ("sti");
}

static inline void hlt(void) {
    __asm__ volatile ("hlt");
}

static inline void pause(void) {
    __asm__ volatile ("pause");
}

// ========== Flags / Barriers ==========

static inline uint32_t cpu_get_flags(void) {
    uint32_t flags;
    __asm__ volatile (
        "pushf\n\t"
        "pop %0"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void memory_barrier(void) {
    __asm__ volatile ("" ::: "memory");
}

#endif // IO_H
