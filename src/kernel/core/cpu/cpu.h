#ifndef CPU_H
#define CPU_H

#include "ktypes.h"

typedef struct {
    char vendor[13];
    char brand[49];
    uint8_t physical_cores;
    uint8_t logical_cores;
    uint8_t threads_per_core;
    uint32_t features_ecx;
    uint32_t features_edx;
    uint32_t extended_features_ebx;
    uint32_t extended_features_ecx;
} cpu_info_t;

void detect_cpu_info(char* cpu_vendor, char* cpu_brand);
void cpu_detect_topology(cpu_info_t* info);
uint8_t cpu_get_core_count(void);
void cpu_print_detailed_info(void);

static inline uint64_t __read_cr2(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline void cpu_cpuid(uint32_t eax, uint32_t ecx, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(eax), "c"(ecx));
}

#endif // CPU_H
