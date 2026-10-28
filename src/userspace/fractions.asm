; Floating point in xmm registers: a square root, the area of a circle, a
; dot product of four floats at once, and a sum of fractions. BoxLib's printf
; has no %f, so each result is scaled and printed as an integer.

section .rodata
align 16
xs      dd 1.0, 2.0, 3.0, 4.0
ys      dd 0.5, 0.25, 2.0, 10.0
one     dq 1.0
two     dq 2.0
radius  dq 10.0
pi      dq 3.141592653589793
hundred dq 100.0
million dq 1000000.0

said_root   db "sqrt(2) = %d / 1000000", 10, 0
said_circle db "a circle of radius 10 has an area of %d / 100", 10, 0
said_dot    db "(1, 2, 3, 4) . (0.5, 0.25, 2, 10) = %d", 10, 0
said_sum    db "1 + 1/2 + 1/3 + ... + 1/10 = %d / 1000000", 10, 0

section .text
extern printf
global main

main:
    push rbx                    ; kept for main's caller; rsp is now aligned to 16

    sqrtsd xmm0, [two]
    mulsd xmm0, [million]
    cvtsd2si esi, xmm0          ; rounded to nearest
    lea rdi, [said_root]
    xor eax, eax
    call printf

    movsd xmm0, [radius]        ; pi * r * r
    mulsd xmm0, xmm0
    mulsd xmm0, [pi]
    mulsd xmm0, [hundred]
    cvtsd2si esi, xmm0
    lea rdi, [said_circle]
    xor eax, eax
    call printf

    movaps xmm0, [xs]           ; four products in one instruction
    mulps xmm0, [ys]
    movaps xmm1, xmm0           ; then the four added across the register
    shufps xmm1, xmm1, 0x4E     ; the halves swapped
    addps xmm0, xmm1
    movaps xmm1, xmm0
    shufps xmm1, xmm1, 0xB1     ; the neighbours swapped
    addps xmm0, xmm1            ; every lane holds the sum
    cvtss2si esi, xmm0
    lea rdi, [said_dot]
    xor eax, eax
    call printf

    xorpd xmm2, xmm2            ; the sum
    mov ebx, 1
.next:
    cvtsi2sd xmm1, ebx
    movsd xmm0, [one]
    divsd xmm0, xmm1
    addsd xmm2, xmm0
    inc ebx
    cmp ebx, 10
    jle .next
    mulsd xmm2, [million]
    cvtsd2si esi, xmm2
    lea rdi, [said_sum]
    xor eax, eax
    call printf

    pop rbx
    box.exit 0
