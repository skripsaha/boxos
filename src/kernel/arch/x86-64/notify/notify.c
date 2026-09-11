#include "notify.h"
#include "gdt.h"
#include "klib.h"

#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_SFMASK          0xC0000084
#define MSR_KERNEL_GS_BASE  0xC0000102

#define EFER_SCE            (1ULL << 0)

#define SFMASK_VALUE        ((1ULL <<  9)  | \
                             (1ULL <<  8)  | \
                             (1ULL << 10)  | \
                             (1ULL << 14)  | \
                             (1ULL << 16)  | \
                             (1ULL << 18) )

static PerCpuData g_per_cpu __attribute__((aligned(16)));

static inline uint64_t rdmsr_local(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_local(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

void notify_init(void) {
    debug_printf("[NOTIFY] Initializing notify...\n");

    uint64_t efer = rdmsr_local(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr_local(MSR_EFER, efer);

    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr_local(MSR_STAR, star);

    wrmsr_local(MSR_LSTAR, (uint64_t)notify_entry);

    wrmsr_local(MSR_SFMASK, SFMASK_VALUE);

    g_per_cpu.kernel_rsp = 0;
    g_per_cpu.user_rsp = 0;
    g_per_cpu.self = (uint64_t)&g_per_cpu;

    wrmsr_local(MSR_KERNEL_GS_BASE, (uint64_t)&g_per_cpu);

    debug_printf("[NOTIFY] STAR=0x%lx LSTAR=0x%lx SFMASK=0x%lx\n",
                 star, (uint64_t)notify_entry, SFMASK_VALUE);
    debug_printf("[NOTIFY] PerCpuData at 0x%lx (KernelGSBASE)\n",
                 (uint64_t)&g_per_cpu);
    debug_printf("[NOTIFY] %[S]Notify enabled!%[D]\n");
}

void notify_set_kernel_rsp(uint64_t rsp) {
    g_per_cpu.kernel_rsp = rsp;
}