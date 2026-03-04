; Entry point for all BoxOS user programs.
; Kernel jumps here after loading the ELF binary.
;
; On entry:
;   - Stack is set up by kernel
;   - Ring buffers are mapped at USER_RINGS_VADDR
;   - argc in RDI, argv in RSI (future)

[BITS 64]

%include "notify.inc"

section .text

extern main
extern exit

global _start

_start:
    xor rbp, rbp        ; clear base pointer for stack traces
    and rsp, -16        ; align stack to 16 bytes (ABI requirement)

    ; TODO: Parse command line from kernel
    xor rdi, rdi        ; argc = 0
    xor rsi, rsi        ; argv = NULL

    call main

    ; exit with return code via C exit() — sends parent notification
    mov edi, eax
    call exit

.halt:
    hlt
    jmp .halt

global exit_asm
exit_asm:
    ; rdi = exit_code
    notify_prepare
    mov eax, NOTIFY_PAGE
    mov word [rax + NP_DATA], di
    notify_prefix DECK_SYSTEM, 0x02
    notify_send
.halt:
    hlt
    jmp .halt
