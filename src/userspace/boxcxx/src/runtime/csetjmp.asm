
[BITS 64]

section .text

%define JB_RBX 0*8
%define JB_RBP 1*8
%define JB_R12 2*8
%define JB_R13 3*8
%define JB_R14 4*8
%define JB_R15 5*8
%define JB_RSP 6*8
%define JB_RIP 7*8
%define JB_SSP 8*8

global __boxcxx_setjmp
global __boxcxx_longjmp

__boxcxx_setjmp:
    endbr64
    mov     [rdi + JB_RBX], rbx
    mov     [rdi + JB_RBP], rbp
    mov     [rdi + JB_R12], r12
    mov     [rdi + JB_R13], r13
    mov     [rdi + JB_R14], r14
    mov     [rdi + JB_R15], r15

    lea     rax, [rsp + 8]
    mov     [rdi + JB_RSP], rax
    mov     rax, [rsp]
    mov     [rdi + JB_RIP], rax

    xor     rax, rax
    rdsspq  rax
    mov     [rdi + JB_SSP], rax

    xor     eax, eax
    ret

__boxcxx_longjmp:
    endbr64
    mov     eax, esi
    test    eax, eax
    jnz     .value_ok
    mov     eax, 1
.value_ok:

    mov     r8, [rdi + JB_SSP]
    test    r8, r8
    jz      .no_shstk
    xor     r9, r9
    rdsspq  r9
    test    r9, r9
    jz      .no_shstk
    mov     r10, 1
.pop_one:
    cmp     r9, r8
    jae     .no_shstk
    incsspq r10
    add     r9, 8
    jmp     .pop_one

.no_shstk:
    mov     rbx, [rdi + JB_RBX]
    mov     rbp, [rdi + JB_RBP]
    mov     r12, [rdi + JB_R12]
    mov     r13, [rdi + JB_R13]
    mov     r14, [rdi + JB_R14]
    mov     r15, [rdi + JB_R15]

    mov     rcx, [rdi + JB_RIP]
    mov     rsp, [rdi + JB_RSP]
    sub     rsp, 8
    mov     [rsp], rcx
    ret