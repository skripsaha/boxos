; userspace.asm - Ring 0 to Ring 3 transition
; BoxOS Snowball Architecture

[BITS 64]

global jump_to_userspace

; void jump_to_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);
; RDI = user RIP (entry point)
; RSI = user RSP (stack pointer)
; RDX = user RFLAGS
jump_to_userspace:
    cli

    mov rcx, rdi
    mov r11, rdx
    sub rsi, 8          ; CRITICAL: Pre-align stack for ABI compliance
    mov rsp, rsi

    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x23
    push rsi
    push r11
    push 0x1B
    push rcx

    iretq
