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

    ; Crossing ring 0 -> ring 3 (single-core initial launch). The kernel runs
    ; with the per-cpu PerCpuData in the active GS base; swapgs parks it in the
    ; shadow (IA32_KERNEL_GS_BASE) so the process's first SYSCALL swaps it back.
    ; The following `mov gs,ax` sets the user GS selector; its descriptor base
    ; is 0, which is exactly the user-mode active GS base we want. The shadow
    ; (per-cpu) is untouched by the selector load.
    swapgs
    mov gs, ax

    push GDT_USER_DATA
    push rsi
    push r11
    push GDT_USER_CODE
    push rcx

    iretq
