#ifndef NOTIFY_H
#define NOTIFY_H

#include "ktypes.h"

// BoxOS Notify — fast kernel entry.
//
// When a Cabin process calls notify(), control transfers to
// notify_entry (notify_entry.asm).  The trampoline swaps to the
// kernel stack via swapgs + PerCpuData, builds an interrupt_frame_t,
// and calls syscall_handler().  INT 0x80 remains as fallback.
//
// Exit uses SYSRETQ (~30 cycles) instead of IRETQ (~80 cycles).
// Canonical-address check guards against CVE-2012-0217.

// Per-CPU data accessed via GS segment after swapgs.
typedef struct {
    uint64_t kernel_rsp;    // offset 0x00: kernel stack top for current process
    uint64_t user_rsp;      // offset 0x08: saved user RSP during notify
    uint64_t self;          // offset 0x10: pointer to self (validation)
} __attribute__((aligned(16))) PerCpuData;

// Initialize notify MSRs and PerCpuData.
void notify_init(void);

// Update the kernel RSP in PerCpuData (call on every context switch).
void notify_set_kernel_rsp(uint64_t rsp);

// Assembly entry point — target of MSR_LSTAR.
extern void notify_entry(void);

#endif // NOTIFY_H
