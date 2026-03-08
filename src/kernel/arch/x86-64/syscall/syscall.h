#ifndef SYSCALL_H
#define SYSCALL_H

#include "ktypes.h"

// BoxOS Fast Notify — SYSCALL/SYSRET kernel entry.
//
// When a Cabin process calls notify(), the CPU executes SYSCALL which
// transfers control to notify_entry (syscall_entry.asm).  The trampoline
// swaps to the kernel stack via swapgs + PerCpuData, builds an
// interrupt_frame_t, and calls syscall_handler() — same handler used
// by the legacy INT 0x80 path.
//
// On return, the fast path uses SYSRETQ (~30 cycles) instead of
// IRETQ (~80 cycles).  A canonical-address check guards against
// CVE-2012-0217.

// Per-CPU data accessed via GS segment after swapgs.
// Only one CPU in BoxOS for now, but the structure is ready.
typedef struct {
    uint64_t kernel_rsp;    // offset 0x00: kernel stack top for current process
    uint64_t user_rsp;      // offset 0x08: saved user RSP during notify
    uint64_t self;          // offset 0x10: pointer to self (validation)
} __attribute__((aligned(16))) PerCpuData;

// Initialize SYSCALL/SYSRET MSRs and PerCpuData.
// Must be called after gdt_init() + tss_init() + idt_init().
void syscall_init(void);

// Update the kernel RSP in PerCpuData (call on every context switch).
void syscall_set_kernel_rsp(uint64_t rsp);

// Assembly entry point — target of MSR_LSTAR.
extern void notify_entry(void);

#endif // SYSCALL_H
