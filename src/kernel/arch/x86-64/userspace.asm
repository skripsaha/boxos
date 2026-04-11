; userspace.asm - Ring 0 to Ring 3 transition

[BITS 64]

%include "gdt_selectors.inc"

global jump_to_userspace

; void jump_to_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);
; RDI = user RIP (entry point)
; RSI = user RSP (stack pointer)
; RDX = user RFLAGS
jump_to_userspace:
    cli

    mov rcx, rdi
    mov r11, rdx
    sub rsi, 8          ; Pre-align stack for ABI compliance
    mov rsp, rsi

    mov ax, GDT_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push GDT_USER_DATA
    push rsi
    push r11
    push GDT_USER_CODE
    push rcx

    iretq
