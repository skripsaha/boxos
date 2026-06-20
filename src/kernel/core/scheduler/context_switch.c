#include "context_switch.h"
#include "scheduler.h"
#include "process.h"
#include "vmm.h"
#include "klib.h"
#include "tss.h"
#include "idt.h"
#include "fpu.h"
#include "notify.h"
#include "per_core.h"

/* The non-frame context_save / context_restore / context_switch helpers
 * have been removed. The scheduler dispatches exclusively via the IRQ
 * frame (`schedule(frame)`), so the pair below is the single live path. */

/* ── User FS base (TLS) save/restore on the SCHEDULER frame path ──────────
 *
 * Strands P5a: each strand owns its own FS base (its StrandInfo / C++ TCB
 * pointer). The asm SAVE/RESTORE_USER_FSBASE macros (context_switch.asm)
 * only cover the task_* entries — NOT this frame path, which is the single
 * live scheduler dispatch. Before P5a there was exactly one FS-base user per
 * cabin so the register survived untouched across switches "by accident";
 * with several strands per cabin, dispatching strand B must install B's FS
 * base, or B would read strand A's TLS (and a fresh strand would never get
 * its own). These helpers mirror the asm macros bit-for-bit:
 *   g_fsgsbase_active → RDFSBASE/WRFSBASE (CR4.FSGSBASE guaranteed enabled)
 *   else g_user_fsbase_used → MSR IA32_FS_BASE (0xC0000100)
 *   else → skip (zero cost until TLS is used)
 *
 * SAFE from C on the frame path: iretq does NOT reload the FS *selector*, so
 * the base written by wrfsbase persists into ring 3 (if it reloaded FS, the
 * descriptor base 0 would wipe it — which is exactly why the asm RESTORE
 * macro must run after the selector load, but the frame path has no such
 * load, so ordering is moot here). ISRs never touch FS, so the value read by
 * the save always belongs to the strand being switched out. */
#define CTX_IA32_FS_BASE_MSR 0xC0000100u

static inline void ctx_save_user_fsbase(ProcessContext* ctx) {
    if (g_fsgsbase_active) {
        uint64_t base;
        __asm__ volatile("rdfsbase %0" : "=r"(base));
        ctx->user_fsbase = base;
    } else if (g_user_fsbase_used) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(CTX_IA32_FS_BASE_MSR));
        ctx->user_fsbase = ((uint64_t)hi << 32) | lo;
    }
}

static inline void ctx_restore_user_fsbase(const ProcessContext* ctx) {
    if (g_fsgsbase_active) {
        __asm__ volatile("wrfsbase %0" : : "r"(ctx->user_fsbase));
    } else if (g_user_fsbase_used) {
        uint32_t lo = (uint32_t)ctx->user_fsbase;
        uint32_t hi = (uint32_t)(ctx->user_fsbase >> 32);
        __asm__ volatile("wrmsr" : : "c"(CTX_IA32_FS_BASE_MSR), "a"(lo), "d"(hi));
    }
}

void context_save_from_frame(process_t* proc, interrupt_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    ProcessContext* ctx = &proc->context;

    // save FPU state before anything else can clobber it
    if (ctx->fpu_state) {
        fpu_save(ctx->fpu_state);
        ctx->fpu_initialized = true;
    }

    // Capture this strand's user FS base (TLS) before it is switched out.
    ctx_save_user_fsbase(ctx);

    ctx->rax = frame->rax;
    ctx->rbx = frame->rbx;
    ctx->rcx = frame->rcx;
    ctx->rdx = frame->rdx;
    ctx->rsi = frame->rsi;
    ctx->rdi = frame->rdi;
    ctx->rbp = frame->rbp;
    ctx->rsp = frame->rsp;
    ctx->r8 = frame->r8;
    ctx->r9 = frame->r9;
    ctx->r10 = frame->r10;
    ctx->r11 = frame->r11;
    ctx->r12 = frame->r12;
    ctx->r13 = frame->r13;
    ctx->r14 = frame->r14;
    ctx->r15 = frame->r15;

    ctx->rip = frame->rip;
    ctx->cs = (uint16_t)frame->cs;
    ctx->ss = (uint16_t)frame->ss;
    ctx->rflags = frame->rflags;

    if (!proc->cabin) {
        return;
    }
    ctx->cr3 = vmm_build_cr3(proc->cabin->vmm);

    proc->started = true;
}

void context_restore_to_frame(process_t* proc, interrupt_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    ProcessContext* ctx = &proc->context;

    /* Skip CR3 reload only on a true address-space match (ignore bit 63
     * NOFLUSH when comparing). On an actual address-space CHANGE, load CR3
     * WITHOUT NOFLUSH, even under PCID.
     *
     * Strands (P2): NOFLUSH preserves this core's TLB entries for the PCID
     * being loaded across the switch — but that is only safe if THIS core
     * last ran this address space AND no unmap of it has happened since,
     * neither of which is tracked. With several strands sharing one cabin
     * (one PCID) across cores, a strand can unmap a page on core A while core
     * B still holds a NOFLUSH-preserved entry for it from an earlier stint;
     * A's CR3-filtered shootdown skips B (B isn't current on the cabin then),
     * so a NOFLUSH reload on B would resurrect the stale entry → use-after-
     * unmap. (Latent even for a single strand that migrates B→A→B across an
     * unmap.) Loading without NOFLUSH invalidates exactly this PCID's entries
     * on this core (other cabins' entries survive), discarding any such stale
     * entry. Two strands of ONE cabin switching on the SAME core hit the
     * equal-CR3 branch above (no CR3 write) and keep the TLB — so the cost is
     * only one per-PCID flush per cross-cabin switch, the price of safety. */
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    const uint64_t CR3_COMPARE_MASK = ~(1ULL << 63);
    if ((current_cr3 & CR3_COMPARE_MASK) != (ctx->cr3 & CR3_COMPARE_MASK)) {
        uint64_t new_cr3 = ctx->cr3 & CR3_COMPARE_MASK;
        __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    }

    frame->rax = ctx->rax;
    frame->rbx = ctx->rbx;
    frame->rcx = ctx->rcx;
    frame->rdx = ctx->rdx;
    frame->rsi = ctx->rsi;
    frame->rdi = ctx->rdi;
    frame->rbp = ctx->rbp;
    frame->rsp = ctx->rsp;
    frame->r8 = ctx->r8;
    frame->r9 = ctx->r9;
    frame->r10 = ctx->r10;
    frame->r11 = ctx->r11;
    frame->r12 = ctx->r12;
    frame->r13 = ctx->r13;
    frame->r14 = ctx->r14;
    frame->r15 = ctx->r15;

    frame->rip = ctx->rip;
    frame->cs = ctx->cs;
    frame->ss = ctx->ss;
    frame->rflags = ctx->rflags;

    if (ctx->fpu_initialized) {
        fpu_restore(ctx->fpu_state);
    }

    // Install the incoming strand's user FS base (TLS). The frame path does
    // not reload the FS selector, so this is the only place per-strand TLS is
    // switched on the live scheduler dispatch.
    ctx_restore_user_fsbase(ctx);
}
