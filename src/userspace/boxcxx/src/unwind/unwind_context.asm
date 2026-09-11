
[BITS 64]

section .text

%define UNW_RAX  0*8
%define UNW_RDX  1*8
%define UNW_RCX  2*8
%define UNW_RBX  3*8
%define UNW_RSI  4*8
%define UNW_RDI  5*8
%define UNW_RBP  6*8
%define UNW_RSP  7*8
%define UNW_R8   8*8
%define UNW_R9   9*8
%define UNW_R10 10*8
%define UNW_R11 11*8
%define UNW_R12 12*8
%define UNW_R13 13*8
%define UNW_R14 14*8
%define UNW_R15 15*8
%define UNW_RIP 16*8

global UnwCaptureContext
UnwCaptureContext:
    endbr64
    mov [rdi + UNW_RAX], rax
    mov [rdi + UNW_RDX], rdx
    mov [rdi + UNW_RCX], rcx
    mov [rdi + UNW_RBX], rbx
    mov [rdi + UNW_RSI], rsi
    mov [rdi + UNW_RDI], rdi
    mov [rdi + UNW_RBP], rbp
    lea rax, [rsp + 8]
    mov [rdi + UNW_RSP], rax
    mov [rdi + UNW_R8],  r8
    mov [rdi + UNW_R9],  r9
    mov [rdi + UNW_R10], r10
    mov [rdi + UNW_R11], r11
    mov [rdi + UNW_R12], r12
    mov [rdi + UNW_R13], r13
    mov [rdi + UNW_R14], r14
    mov [rdi + UNW_R15], r15
    mov rax, [rsp]
    mov [rdi + UNW_RIP], rax
    mov rax, [rdi + UNW_RAX]
    ret

global UnwRestoreContext
UnwRestoreContext:
    endbr64
    mov rbx, [rdi + UNW_RBX]
    mov rbp, [rdi + UNW_RBP]
    mov r12, [rdi + UNW_R12]
    mov r13, [rdi + UNW_R13]
    mov r14, [rdi + UNW_R14]
    mov r15, [rdi + UNW_R15]
    mov rax, [rdi + UNW_RAX]
    mov rdx, [rdi + UNW_RDX]
    mov rcx, [rdi + UNW_RIP]
    mov rsp, [rdi + UNW_RSP]
    notrack jmp rcx