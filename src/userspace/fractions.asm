; Floating point in xmm registers: a square root, the area of a circle, a
; dot product of four floats at once, and a sum of fractions, printed by
; printf's %f, %g and %e. A double reaches printf in xmm0, xmm1 and on, and al
; says how many vector registers carry one; a float is widened to a double.

section .rodata
align 16
xs      dd 1.0, 2.0, 3.0, 4.0
ys      dd 0.5, 0.25, 2.0, 10.0
one     dq 1.0
two     dq 2.0
radius  dq 10.0
pi      dq 3.141592653589793

said_root   db "sqrt(2) = %.15f", 10, 0
said_circle db "a circle of radius %g has an area of %.4f", 10, 0
said_dot    db "(1, 2, 3, 4) . (0.5, 0.25, 2, 10) = %g", 10, 0
said_sum    db "1 + 1/2 + 1/3 + ... + 1/10 = %.17g (%e)", 10, 0

section .text
extern printf
global main

main:
    push rbx                    ; kept for main's caller; rsp is now aligned to 16

    sqrtsd xmm0, [two]
    lea rdi, [said_root]
    mov eax, 1                  ; one vector register carries an argument
    call printf

    movsd xmm0, [radius]        ; the radius, then pi * r * r
    movsd xmm1, xmm0
    mulsd xmm1, xmm1
    mulsd xmm1, [pi]
    lea rdi, [said_circle]
    mov eax, 2
    call printf

    movaps xmm0, [xs]           ; four products in one instruction
    mulps xmm0, [ys]
    movaps xmm1, xmm0           ; then the four added across the register
    shufps xmm1, xmm1, 0x4E     ; the halves swapped
    addps xmm0, xmm1
    movaps xmm1, xmm0
    shufps xmm1, xmm1, 0xB1     ; the neighbours swapped
    addps xmm0, xmm1            ; every lane holds the sum
    cvtss2sd xmm0, xmm0         ; printf takes a double
    lea rdi, [said_dot]
    mov eax, 1
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
    movsd xmm0, xmm2
    movsd xmm1, xmm2
    lea rdi, [said_sum]
    mov eax, 2
    call printf

    pop rbx
    box.exit 0
