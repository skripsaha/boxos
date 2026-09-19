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
// Exit is IRETQ — the one at the end of notify_entry.asm, on every return,
// with no SYSRETQ path anywhere in this kernel.  Not an oversight: SYSRETQ
// returns to the RIP it finds in RCX, so taking it would mean overwriting
// RCX with the frame's RIP, and RCX holds live user data whenever the frame
// being returned to was built by a timer interrupt rather than by SYSCALL.
// notify_entry.asm keeps the same reasoning at the instruction itself, with
// the crash it once caused.
//
// So there is no canonical-address test on any return path here, and none is
// owed: CVE-2012-0217 is a SYSRET hazard — SYSRET with a non-canonical return
// RIP raises #GP in ring 0 on the user's stack — and an IRETQ return is not
// exposed to it.  Anything that ever converts this exit to SYSRETQ has to add
// that test (return RIP against the canonical boundary, IRETQ when it is
// outside) together with RCX = return RIP and R11 = RFLAGS.

// Per-CPU data accessed via GS segment.
//
// GS-base invariant (BoxOS, established in per_core.c):
//   - kernel mode: active GS.base (IA32_GS_BASE) -> this struct; the shadow
//     (IA32_KERNEL_GS_BASE) holds the user GS base.
//   - user mode:   active GS.base -> user value; shadow -> this struct.
// Every ring crossing flips them with swapgs (isr.asm / notify_entry.asm /
// jump_to_userspace), so kernel code can read per-cpu fields via %gs in ANY
// context (IRQ, exception, syscall, thread). core_index is read on the hot
// scheduler path by amp_get_core_index().
typedef struct {
    uint64_t kernel_rsp;    // offset 0x00: kernel stack top for current process
    uint64_t user_rsp;      // offset 0x08: saved user RSP during notify
    uint64_t self;          // offset 0x10: pointer to self (validation)
    uint32_t core_index;    // offset 0x18: this CPU's dense core index
    uint32_t _pad;          // offset 0x1C: pad to 8-byte boundary
} __attribute__((aligned(16))) PerCpuData;

// Initialize notify MSRs and PerCpuData.
void notify_init(void);

// Update the kernel RSP in PerCpuData (call on every context switch).
void notify_set_kernel_rsp(uint64_t rsp);

// Assembly entry point — target of MSR_LSTAR.
extern void notify_entry(void);

#endif // NOTIFY_H
