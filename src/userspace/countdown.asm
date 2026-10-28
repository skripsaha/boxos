; countdown — a BoxASM program: data, a loop, a function of its own,
; BoxLib's printf, and the box operations.

section .rodata
title   db "BoxASM counts down", 10, 0
line    db "  %d", 10, 0
total   db "1 + 2 + ... + 100 = %d", 10, 0

section .text
extern printf
global main

main:
    push rbx                ; kept for main's caller; rsp is now aligned to 16
    box.say title
    mov ebx, 5
.next:
    lea rdi, [line]
    mov esi, ebx
    xor eax, eax            ; printf takes varargs: al counts vector registers
    call printf
    dec ebx
    jnz .next
    mov edi, 100
    call sum_to
    lea rdi, [total]
    mov esi, eax
    xor eax, eax
    call printf
    box.say "liftoff!\n"
    pop rbx
    box.exit 0

; eax = 1 + 2 + ... + edi, for edi > 0
sum_to:
    xor eax, eax
.add:
    add eax, edi
    dec edi
    jnz .add
    ret
