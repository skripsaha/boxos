; The preprocessor at work: a table of squares built by %rep, rectangles laid
; out with struc and istruc, the largest found by a macro with a label of its
; own, and printf called through a macro that takes one to four parameters.

%define SQUARES 8               ; how many squares the table holds
%define SHOW_TOTAL 1            ; 0 leaves the totals out

; say FORMAT [, a [, b [, c]]]: up to three 32-bit integers for printf, which
; takes no vector registers here, so al is 0.
%macro say 1-4
    lea rdi, [%1]
%if %0 >= 2
    mov esi, %2
%endif
%if %0 >= 3
    mov edx, %3
%endif
%if %0 >= 4
    mov ecx, %4
%endif
    xor eax, eax
    call printf
%endmacro

; larger DEST, SOURCE: DEST becomes the larger of the two, signed.
%macro larger 2
    cmp %1, %2
    jge %%kept
    mov %1, %2
%%kept:
%endmacro

struc rect
.width:  resd 1
.height: resd 1
endstruc

; rectangle WIDTH [, HEIGHT]: one rect, its height 1 unless it is said.
%macro rectangle 1-2 1
    istruc rect
        at rect.width, dd %1
        at rect.height, dd %2
    iend
%endmacro

section .rodata
said_square db "%d squared is %d", 10, 0
said_rect   db "a %d by %d rectangle covers %d", 10, 0
said_total  db "together they cover %d, the largest %d", 10, 0

section .data
align 4
squares:
%assign n 1
%rep SQUARES
    dd n * n
%assign n n + 1
%endrep

rects:
    rectangle 3, 4
    rectangle 5, 5
    rectangle 7                 ; a strip
rects_end:

section .text
extern printf
global main

main:
    push rbx
    push r12
    push r13
    push r14
    push r15                    ; five pushes: rsp is aligned to 16 again

    lea rbx, [squares]
    xor r12d, r12d
.square:
    lea r13d, [r12 + 1]
    say said_square, r13d, [rbx + r12*4]
    inc r12d
    cmp r12d, SQUARES
    jb .square

    lea rbx, [rects]
    xor r14d, r14d              ; the total
    xor r15d, r15d              ; the largest
.rect:
    mov r13d, [rbx + rect.width]
    imul r13d, [rbx + rect.height]
    add r14d, r13d
    larger r15d, r13d
    say said_rect, [rbx + rect.width], [rbx + rect.height], r13d
    add rbx, rect_size
    lea rax, [rects_end]
    cmp rbx, rax
    jb .rect
%if SHOW_TOTAL
    say said_total, r14d, r15d
%endif

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    box.exit 0
