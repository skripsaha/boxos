; Entry point for all BoxOS user programs.
; Kernel jumps here after loading the ELF binary.
;
; On entry:
;   - Stack is set up by kernel
;   - Ring buffers are mapped at USER_RINGS_VADDR
;   - argc in RDI, argv in RSI (future)

[BITS 64]

section .text

extern main

global _start

_start:
    xor rbp, rbp        ; clear base pointer for stack traces
    and rsp, -16        ; align stack to 16 bytes (ABI requirement)

    ; TODO: Parse command line from kernel
    xor rdi, rdi        ; argc = 0
    xor rsi, rsi        ; argv = NULL

    call main

    ; exit with return code (NOTIFY_EXIT = 0x10)
    mov rdi, rax
    mov rsi, rax
    mov rdi, 0x10
    int 0x80

.halt:
    hlt
    jmp .halt

global exit_asm
exit_asm:
    mov rsi, rdi
    mov rdi, 0x10
    int 0x80
.halt:
    hlt
    jmp .halt
