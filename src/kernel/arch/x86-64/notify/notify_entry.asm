; syscall_entry.asm — BoxOS fast notify entry via SYSCALL/SYSRET.
;
; When a Cabin process calls notify(), the CPU executes SYSCALL:
;   RCX = return RIP
;   R11 = saved RFLAGS
;   CS/SS = kernel selectors (from MSR_STAR)
;   RFLAGS &= ~SFMASK  (IF, TF, DF cleared)
;   RSP is NOT switched — we do it manually via swapgs + PerCpuData.
;
; We build an interrupt_frame_t on the kernel stack and call the same
; syscall_handler() used by the legacy INT 0x80 path, so Guide/Deck
; processing is identical regardless of entry method.
;
; Exit: SYSRETQ fast path (~30 cycles) with IRETQ fallback for safety.

[BITS 64]

%include "gdt_selectors.inc"

; PerCpuData field offsets (must match syscall.h PerCpuData struct)
%define PERCPU_KERNEL_RSP  0x00
%define PERCPU_USER_RSP    0x08

; Notify vector — matches CONFIG_SYSCALL_VECTOR (0x80) so the handler
; sees the same vector whether entry was via SYSCALL or INT 0x80.
%define NOTIFY_VECTOR      128

section .text

extern syscall_handler

global notify_entry

; =============================================================================
; notify_entry — SYSCALL landing pad
; =============================================================================
notify_entry:
    ; --- Switch to kernel stack ---
    swapgs                              ; GS now -> PerCpuData
    mov [gs:PERCPU_USER_RSP], rsp       ; save user RSP
    mov rsp, [gs:PERCPU_KERNEL_RSP]     ; load kernel RSP

    ; --- Build interrupt_frame_t (must match isr_common layout in isr.asm) ---
    ; Bottom of frame: what IRETQ expects (ss, rsp, rflags, cs, rip)
    push GDT_USER_DATA                  ; ss
    push qword [gs:PERCPU_USER_RSP]     ; rsp (user)
    push r11                            ; rflags (SYSCALL saved original in R11)
    push GDT_USER_CODE                  ; cs
    push rcx                            ; rip (SYSCALL saved return addr in RCX)

    ; vector + error_code
    push 0                              ; error_code (none)
    push NOTIFY_VECTOR                  ; vector = 0x80

    ; General-purpose registers (same order as isr_common)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; --- Call handler ---
    mov rdi, rsp                        ; arg0 = interrupt_frame_t*

    ; 16-byte stack alignment (System V ABI)
    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax                            ; save alignment correction

    call syscall_handler

    ; Restore alignment
    pop rax
    add rsp, rax

    ; --- Restore registers from (possibly modified) frame ---
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                         ; skip vector + error_code

    ; =================================================================
    ; Return to user mode.
    ;
    ; IRETQ is always safe: it restores RIP, CS, RFLAGS, RSP and SS from
    ; the hardware frame on the stack, and all GPRs have already been
    ; restored by the pops above.  In particular, RCX is the value the
    ; user process had when it was last saved — which may be a live data
    ; pointer (e.g. a memset loop counter) rather than a return address.
    ;
    ; Why NOT SYSRETQ here:
    ;   SYSRETQ requires that RCX already hold the return RIP before the
    ;   instruction executes.  To set up RCX we would have to overwrite it
    ;   with [frame->rip].  If the process was timer-interrupted (not via
    ;   SYSCALL), its live RCX is a data register and overwriting it with
    ;   a code address causes the next pointer-dereference to land inside
    ;   .text → write page-fault.  That was the root cause of the
    ;   RCX=0x10e70 / CR2=weird crash pattern (audit 2026-04-30).
    ;
    ;   Performance: IRETQ costs ≈ 30-50 cycles more than SYSRETQ but the
    ;   notify path already pays hundreds of cycles for guide() processing,
    ;   so the difference is negligible.
    ; =================================================================

    ; Conditional swapgs: syscall_handler may have rescheduled to a ring-0
    ; kernel process (e.g. the idle task), in which case the rewritten frame
    ; carries a kernel CS and we must KEEP the per-cpu GS base. Only swap back
    ; to the user GS base when actually returning to ring 3.
    ; After `add rsp,16` the stack top is the CPU frame: rip(+0) cs(+8) ...
    test byte [rsp+8], 3                ; CS.RPL == 3 ?  (returning to user)
    jz .notify_ret_kernel
    swapgs
.notify_ret_kernel:
    iretq
