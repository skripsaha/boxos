
[BITS 64]

section .text

extern main
extern exit
extern __box_runtime_init

global _start

_start:
    endbr64
    xor rbp, rbp
    and rsp, -16

    call __box_runtime_init

    xor rdi, rdi
    xor rsi, rsi

    call main

    mov edi, eax
    call exit

.halt:
    hlt
    jmp .halt