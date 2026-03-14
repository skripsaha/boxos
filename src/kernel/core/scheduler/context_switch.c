#include "context_switch.h"
#include "scheduler.h"
#include "process.h"
#include "vmm.h"
#include "klib.h"
#include "tss.h"
#include "idt.h"
#include "fpu.h"
#include "notify.h"

void context_save(process_t* proc, ProcessContext* ctx) {
    if (!proc || !ctx) {
        return;
    }

    fpu_save(ctx->fpu_state);
    ctx->fpu_initialized = true;

    if (!proc->cabin) {
        return;
    }

    ctx->cr3 = vmm_build_cr3(proc->cabin);
}

void context_restore(process_t* proc, ProcessContext* ctx) {
    if (!proc || !ctx) {
        return;
    }

    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != ctx->cr3) {
        uint64_t new_cr3 = ctx->cr3;
        if (vmm_pcid_active()) new_cr3 |= (1ULL << 63);  // NOFLUSH
        __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    }

    if (ctx->fpu_initialized) {
        fpu_restore(ctx->fpu_state);
    }

    tss_set_rsp0((uint64_t)proc->kernel_stack_top);
    notify_set_kernel_rsp((uint64_t)proc->kernel_stack_top);
}

void context_switch(process_t* from, process_t* to) {
    if (from) {
        context_save(from, &from->context);
    }

    if (to) {
        context_restore(to, &to->context);
        if (process_get_state(to) == PROC_CREATED) {
            process_set_state(to, PROC_WORKING);
        }
        to->last_run_time = scheduler_get_state()->total_ticks;
        scheduler_state_t* sched = scheduler_get_state();
        spin_lock(&sched->scheduler_lock);
        sched->current_process = to;
        spin_unlock(&sched->scheduler_lock);
    }
}

void context_save_from_frame(process_t* proc, interrupt_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    ProcessContext* ctx = &proc->context;

    // save FPU state before anything else can clobber it
    fpu_save(ctx->fpu_state);
    ctx->fpu_initialized = true;

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
    ctx->cr3 = vmm_build_cr3(proc->cabin);

    proc->started = true;
}

void context_restore_to_frame(process_t* proc, interrupt_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    ProcessContext* ctx = &proc->context;

    // Skip CR3 reload if returning to same address space
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != ctx->cr3) {
        uint64_t new_cr3 = ctx->cr3;
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
