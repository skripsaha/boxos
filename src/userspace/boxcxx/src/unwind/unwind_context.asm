; boxcxx — register-context capture/restore for the DWARF unwinder.
;
; UnwRegisterFile layout (must match unwind_level1.cpp):
;   17 x uint64_t, DWARF x86-64 numbering:
;     0=rax 1=rdx 2=rcx 3=rbx 4=rsi 5=rdi 6=rbp 7=rsp 8..15=r8..r15 16=rip
;
; CET notes:
;   - Every entry starts with ENDBR64 (IA32_U_CET.ENDBR_EN=1 in BoxOS).
;   - The final transfer is a NOTRACK indirect jmp: landing pads are
;     reached by unwinding, not by a tracked branch, and NO_TRACK_EN=1
;     permits the prefix. A `ret` would consume the HANDLER frame's
;     shadow-stack entry and #CP — never return here.
;   - Shadow-stack reconciliation (INCSSPQ over the skipped frames) is
;     the caller's job (Phase 4B) BEFORE invoking the restore.

[BITS 64]

section .text

%define UNW_RAX  0*8
%define UNW_RDX  1*8
%define UNW_RCX  2*8
%define UNW_RBX  3*8
%define UNW_RSI  4*8
%define UNW_RDI  5*8
%define UNW_RBP  6*8
%define UNW_RSP  7*8
%define UNW_R8   8*8
%define UNW_R9   9*8
%define UNW_R10 10*8
%define UNW_R11 11*8
%define UNW_R12 12*8
%define UNW_R13 13*8
%define UNW_R14 14*8
%define UNW_R15 15*8
%define UNW_RIP 16*8

;------------------------------------------------------------------------
; void UnwCaptureContext(UnwRegisterFile *out)   ; rdi = out
;
; Captures the CALLER's view: RSP as it will be after our return (rsp+8),
; RIP = our return address, every other register as live at the call.
;------------------------------------------------------------------------
global UnwCaptureContext
UnwCaptureContext:
    endbr64
    mov [rdi + UNW_RAX], rax
    mov [rdi + UNW_RDX], rdx
    mov [rdi + UNW_RCX], rcx
    mov [rdi + UNW_RBX], rbx
    mov [rdi + UNW_RSI], rsi
    mov [rdi + UNW_RDI], rdi
    mov [rdi + UNW_RBP], rbp
    lea rax, [rsp + 8]
    mov [rdi + UNW_RSP], rax
    mov [rdi + UNW_R8],  r8
    mov [rdi + UNW_R9],  r9
    mov [rdi + UNW_R10], r10
    mov [rdi + UNW_R11], r11
    mov [rdi + UNW_R12], r12
    mov [rdi + UNW_R13], r13
    mov [rdi + UNW_R14], r14
    mov [rdi + UNW_R15], r15
    mov rax, [rsp]
    mov [rdi + UNW_RIP], rax
    mov rax, [rdi + UNW_RAX]     ; rax stays unclobbered for the caller
    ret

;------------------------------------------------------------------------
; [[noreturn]] void UnwRestoreContext(const UnwRegisterFile *ctx) ; rdi
;
; Installs the target frame's register state and jumps to ctx->rip (the
; landing pad). System V EH contract at a landing pad: only the frame's
; callee-saved registers, RSP and the eh-return data registers (RAX,RDX)
; are live — every caller-saved register is dead, so RCX is free scratch
; for the target IP.
;------------------------------------------------------------------------
global UnwRestoreContext
UnwRestoreContext:
    endbr64
    mov rbx, [rdi + UNW_RBX]
    mov rbp, [rdi + UNW_RBP]
    mov r12, [rdi + UNW_R12]
    mov r13, [rdi + UNW_R13]
    mov r14, [rdi + UNW_R14]
    mov r15, [rdi + UNW_R15]
    mov rax, [rdi + UNW_RAX]
    mov rdx, [rdi + UNW_RDX]
    mov rcx, [rdi + UNW_RIP]
    mov rsp, [rdi + UNW_RSP]
    notrack jmp rcx
