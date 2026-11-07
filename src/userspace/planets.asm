; The planets in their order from the Sun, from three other files. The table
; is planets.inc, beside this one in the context. Every line is said by
; lines.inc, a small library kept apart from the examples and wearing
; asmlib: `everywhere` reaches past the context to it, and the library then
; finds its own part, lines_core.inc, among asmlib.

%include "lines.inc" asmlib everywhere
%include "planets.inc"

section .text
global main

main:
    push rbx
    push r12
    push r13                    ; three pushes: rsp is aligned to 16 again

    lea r12, [planet_names]
    xor ebx, ebx
.planet:
    mov r13, [r12 + rbx * 8]
    lea rax, [rbx + 1]
    line "%s is planet %d", r13, rax
    inc ebx
    cmp ebx, PLANETS
    jb .planet

    line "%d planets, counted from the Sun", PLANETS

    pop r13
    pop r12
    pop rbx
    xor eax, eax
    ret
