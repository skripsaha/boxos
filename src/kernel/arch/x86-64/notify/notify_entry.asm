
[BITS 64]

%include "gdt_selectors.inc"

%define PERCPU_KERNEL_RSP  0x00
%define PERCPU_USER_RSP    0x08

%define NOTIFY_VECTOR      128

section .text

extern syscall_handler

global notify_entry

notify_entry:
    endbr64
    swapgs
    mov [gs:PERCPU_USER_RSP], rsp
    mov rsp, [gs:PERCPU_KERNEL_RSP]

    push GDT_USER_DATA
    push qword [gs:PERCPU_USER_RSP]
    push r11
    push GDT_USER_CODE
    push rcx

    push 0
    push NOTIFY_VECTOR

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp

    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax

    call syscall_handler

    pop rax
    add rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16


    test byte [rsp+8], 3
    jz .notify_ret_kernel
    swapgs
.notify_ret_kernel:
    iretq