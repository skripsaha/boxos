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

    /* Skip CR3 reload only on a true address-space match. Same caveat as
     * context_restore above: ignore bit 63 (NOFLUSH) when comparing. */
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    const uint64_t CR3_COMPARE_MASK = ~(1ULL << 63);
    if ((current_cr3 & CR3_COMPARE_MASK) != (ctx->cr3 & CR3_COMPARE_MASK)) {
        uint64_t new_cr3 = ctx->cr3 & CR3_COMPARE_MASK;
        if (vmm_pcid_active()) new_cr3 |= (1ULL << 63);  // NOFLUSH
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
}
