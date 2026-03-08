; Entry point for all BoxOS user programs.
; Kernel jumps here after loading the ELF binary.
;
; On entry:
;   - Stack is set up by kernel
;   - PocketRing mapped at 0x2000, ResultRing at 0x3000
;   - CabinInfo at 0x1000

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
    ; Build a Pocket on stack: kill self (DECK_SYSTEM, opcode 0x02)
    sub rsp, 96
    mov rsi, rsp
    ; Zero it
    push rcx
    mov ecx, 96
.zero:
    mov byte [rsi + rcx - 1], 0
    dec ecx
    jnz .zero
    pop rcx
    ; Set prefix_count = 1
    mov byte [rsp + PKT_PREFIX_COUNT], 1
    ; Set prefix[0] = (DECK_SYSTEM << 8) | 0x02 = 0xFF02
    mov word [rsp + PKT_PREFIXES], 0xFF02
    ; Push to PocketRing
    mov rdi, rsp
    pocket_push
    add rsp, 96
    ; notify — tell kernel to process our exit Pocket
    syscall
.halt_exit:
    hlt
    jmp .halt_exit
