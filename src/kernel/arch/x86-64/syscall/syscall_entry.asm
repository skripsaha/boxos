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
    ; SYSRETQ fast path — only safe if returning to user code.
    ;
    ; Guard 1: CS must be user code (schedule() may have switched process,
    ;          or we may return to idle which runs in ring 0).
    ; Guard 2: RIP must be canonical (CVE-2012-0217 — SYSRETQ with
    ;          non-canonical RCX causes #GP in ring 0, game over).
    ; =================================================================

    ; Stack now: rip, cs, rflags, rsp, ss
    cmp qword [rsp + 8], GDT_USER_CODE
    jne .slow_exit

    ; Canonical check: user addresses have bit 47 = 0
    mov rcx, [rsp]                      ; load return RIP
    bt  rcx, 47
    jc  .slow_exit                      ; bit 47 set → non-canonical, bail

    ; --- SYSRETQ fast path ---
    mov r11, [rsp + 16]                 ; RFLAGS → R11 (SYSRETQ restores from R11)
    mov rsp, [rsp + 24]                 ; user RSP

    swapgs                              ; back to user GS
    o64 sysret                          ; → Ring 3 at RCX with RFLAGS from R11

.slow_exit:
    ; --- IRETQ fallback (handles kernel return, non-canonical, etc.) ---
    swapgs
    iretq
