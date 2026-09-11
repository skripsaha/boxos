
[BITS 64]

%include "gdt_selectors.inc"

global jump_to_userspace

jump_to_userspace:
    endbr64
    cli

    mov ax, GDT_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax

    swapgs
    mov gs, ax

    push GDT_USER_DATA
    push rsi
    push rdx
    push GDT_USER_CODE
    push rdi

    iretq