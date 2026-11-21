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
;
; CET / IBT note: SYSCALL itself does NOT set WAIT_FOR_ENDBRANCH per
; Intel SDM Vol 3D §17.3.3 (the implicit set list excludes SYSCALL),
; but the symbol's address is taken via `wrmsr_local(MSR_LSTAR,
; (uint64_t)notify_entry)` and the C compiler under -fcf-protection=full
; emits ENDBR64 at every address-taken C function. We mirror that in
; asm: ENDBR64 here is defense-in-depth, matches Linux's
; entry_SYSCALL_64 convention, and is a NOP when CET is off.
; =============================================================================
notify_entry:
    endbr64
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
    ; Return.
    ;
    ; SYSRETQ wherever it restores exactly what IRETQ would, IRETQ elsewhere.
    ; SYSRETQ takes RIP from RCX and RFLAGS from R11, loads the user
    ; selectors STAR names (CS 0x23, SS 0x1B) and leaves RSP alone. So:
    ;
    ;   - to ring 3, with those very selectors in the frame;
    ;   - the frame's own RCX equal to its RIP and its R11 to its RFLAGS —
    ;     then setting RIP and RFLAGS from them loses nothing the strand
    ;     holds. A strand that came in through SYSCALL and goes back to its
    ;     next instruction has them so (SYSCALL put them there); one that was
    ;     interrupted, whose frame schedule() put here, almost never, and
    ;     takes IRETQ. SYSRETQ used to load RCX from the frame's RIP for every
    ;     return, and a strand whose live RCX was a pointer went on with a
    ;     code address in it (RCX=0x10e70 / CR2 crash, audit 2026-04-30);
    ;   - no RF or VM in RFLAGS, which SYSRETQ clears;
    ;   - a RIP in the lower half: SYSRETQ to a non-canonical RCX faults in
    ;     ring 0 on Intel, on the user's stack (CVE-2012-0217). The look
    ;     shifts RCX and pops it back: POP leaves the flags alone.
    ;
    ; The user RSP is loaded last, interrupts off; NMI, #MC, #DB, #DF and #SS
    ; run on stacks of their own (IST), so nothing lands on the user's stack
    ; in ring 0.
    ; =================================================================
    cmp qword [rsp+8], GDT_USER_CODE    ; cs
    jne .notify_iret
    cmp qword [rsp+32], GDT_USER_DATA   ; ss
    jne .notify_iret
    cmp rcx, [rsp]                      ; rcx == rip
    jne .notify_iret
    cmp r11, [rsp+16]                   ; r11 == rflags
    jne .notify_iret
    test r11, (1 << 16) | (1 << 17)     ; RF | VM
    jnz .notify_iret
    push rcx
    shr rcx, 47                         ; lower half: bits 63..47 clear
    pop rcx
    jnz .notify_iret
    cli
    swapgs
    mov rsp, [rsp+24]                   ; user rsp
    o64 sysret

    ; Conditional swapgs: syscall_handler may have rescheduled to a ring-0
    ; kernel process (e.g. the idle task), in which case the rewritten frame
    ; carries a kernel CS and we must KEEP the per-cpu GS base. Only swap back
    ; to the user GS base when actually returning to ring 3.
    ; After `add rsp,16` the stack top is the CPU frame: rip(+0) cs(+8) ...
.notify_iret:
    test byte [rsp+8], 3                ; CS.RPL == 3 ?  (returning to user)
    jz .notify_ret_kernel
    swapgs
.notify_ret_kernel:
    iretq
