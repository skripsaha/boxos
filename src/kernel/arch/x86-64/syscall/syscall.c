#include "syscall.h"
#include "gdt.h"
#include "klib.h"

// x86-64 MSRs for SYSCALL/SYSRET
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_SFMASK          0xC0000084
#define MSR_KERNEL_GS_BASE  0xC0000102

#define EFER_SCE            (1ULL << 0)   // SYSCALL Enable

// SFMASK: bits cleared in RFLAGS on SYSCALL entry
// IF=9 (disable interrupts), TF=8 (no single-step), DF=10 (clear direction)
#define SFMASK_VALUE        ((1ULL << 9) | (1ULL << 8) | (1ULL << 10))

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

void syscall_init(void) {
    debug_printf("[NOTIFY] Initializing fast notify (SYSCALL/SYSRET)...\n");

    // Enable SYSCALL/SYSRET in EFER
    uint64_t efer = rdmsr_local(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr_local(MSR_EFER, efer);

    // STAR MSR: kernel CS/SS in [47:32], SYSRET base in [63:48]
    //   SYSCALL loads: CS = STAR[47:32], SS = STAR[47:32] + 8
    //   SYSRET  loads: SS = STAR[63:48] + 8, CS = STAR[63:48] + 16  (64-bit mode)
    //
    //   Kernel: CS=0x08 SS=0x10  →  STAR[47:32] = 0x08
    //   User:   SS=0x1B CS=0x23  →  STAR[63:48] = 0x10  (0x10+8=0x18|3, 0x10+16=0x20|3)
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr_local(MSR_STAR, star);

    // LSTAR: notify entry point (SYSCALL jumps here)
    wrmsr_local(MSR_LSTAR, (uint64_t)notify_entry);

    // SFMASK: clear IF, TF, DF on entry — kernel runs with interrupts disabled
    wrmsr_local(MSR_SFMASK, SFMASK_VALUE);

    // Initialize PerCpuData
    g_per_cpu.kernel_rsp = 0;
    g_per_cpu.user_rsp = 0;
    g_per_cpu.self = (uint64_t)&g_per_cpu;

    // Set KernelGSBASE — swapgs will load this into GSBASE on SYSCALL entry
    wrmsr_local(MSR_KERNEL_GS_BASE, (uint64_t)&g_per_cpu);

    debug_printf("[NOTIFY] STAR=0x%lx LSTAR=0x%lx SFMASK=0x%lx\n",
                 star, (uint64_t)notify_entry, SFMASK_VALUE);
    debug_printf("[NOTIFY] PerCpuData at 0x%lx (KernelGSBASE)\n",
                 (uint64_t)&g_per_cpu);
    debug_printf("[NOTIFY] %[S]Fast notify enabled!%[D]\n");
}

void syscall_set_kernel_rsp(uint64_t rsp) {
    g_per_cpu.kernel_rsp = rsp;
}
