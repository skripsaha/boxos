[BITS 64]

section .text

global task_save_context
task_save_context:
    mov [rdi + 0],  rax
    mov [rdi + 8],  rbx
    mov [rdi + 16], rcx
    mov [rdi + 24], rdx
    mov [rdi + 32], rsi
    mov [rdi + 40], rdi
    mov [rdi + 48], rbp
    mov [rdi + 56], rsp
    mov [rdi + 64], r8
    mov [rdi + 72], r9
    mov [rdi + 80], r10
    mov [rdi + 88], r11
    mov [rdi + 96], r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    ; Save RIP from return address on stack
    mov rax, [rsp]
    mov [rdi + 128], rax

    pushfq
    pop rax
    mov [rdi + 136], rax

    mov ax, cs
    mov [rdi + 144], ax
    mov ax, ds
    mov [rdi + 146], ax
    mov ax, es
    mov [rdi + 148], ax
    mov ax, fs
    mov [rdi + 150], ax
    mov ax, gs
    mov [rdi + 152], ax
    mov ax, ss
    mov [rdi + 154], ax

    ; FPU/SSE state saved by C caller via fxsave/fxrstor
    ret

global task_restore_context
task_restore_context:
    mov ax, [rdi + 146]
    mov ds, ax
    mov ax, [rdi + 148]
    mov es, ax
    mov ax, [rdi + 150]
    mov fs, ax
    mov ax, [rdi + 152]
    mov gs, ax
    mov ax, [rdi + 154]
    mov ss, ax

    mov rax, [rdi + 136]
    push rax
    popfq

    mov rsp, [rdi + 56]

    mov rbx, [rdi + 8]
    mov rcx, [rdi + 16]
    mov rdx, [rdi + 24]
    mov rsi, [rdi + 32]
    mov rbp, [rdi + 48]
    mov r8,  [rdi + 64]
    mov r9,  [rdi + 72]
    mov r10, [rdi + 80]
    mov r11, [rdi + 88]
    mov r12, [rdi + 96]
    mov r13, [rdi + 104]
    mov r14, [rdi + 112]
    mov r15, [rdi + 120]

    mov rax, [rdi + 128]
    push rax

    ; Restore rax and rdi last
    mov rax, [rdi + 0]
    mov rdi, [rdi + 40]

    ret

global task_switch_to
task_switch_to:
    mov [rdi + 0],  rax
    mov [rdi + 8],  rbx
    mov [rdi + 16], rcx
    mov [rdi + 24], rdx
    mov [rdi + 32], rsi
    mov [rdi + 40], rdi
    mov [rdi + 48], rbp
    mov [rdi + 56], rsp
    mov [rdi + 64], r8
    mov [rdi + 72], r9
    mov [rdi + 80], r10
    mov [rdi + 88], r11
    mov [rdi + 96], r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    mov rax, [rsp]
    mov [rdi + 128], rax

    pushfq
    pop rax
    mov [rdi + 136], rax

    mov ax, cs
    mov [rdi + 144], ax
    mov ax, ds
    mov [rdi + 146], ax
    mov ax, es
    mov [rdi + 148], ax
    mov ax, fs
    mov [rdi + 150], ax
    mov ax, gs
    mov [rdi + 152], ax
    mov ax, ss
    mov [rdi + 154], ax

    mov ax, [rsi + 146]
    mov ds, ax
    mov ax, [rsi + 148]
    mov es, ax
    mov ax, [rsi + 150]
    mov fs, ax
    mov ax, [rsi + 152]
    mov gs, ax
    mov ax, [rsi + 154]
    mov ss, ax

    mov rax, [rsi + 136]
    push rax
    popfq

    mov rsp, [rsi + 56]

    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    mov rbp, [rsi + 48]
    mov r8,  [rsi + 64]
    mov r9,  [rsi + 72]
    mov r10, [rsi + 80]
    mov r11, [rsi + 88]
    mov r12, [rsi + 96]
    mov r13, [rsi + 104]
    mov r14, [rsi + 112]
    mov r15, [rsi + 120]

    mov rax, [rsi + 128]
    push rax

    ; Restore rax, rsi, rdi last
    mov rax, [rsi + 0]
    mov rdi, [rsi + 40]
    mov rsi, [rsi + 32]

    ret

global task_init_context
task_init_context:
    xor rax, rax
    mov [rdi + 0],  rax
    mov [rdi + 8],  rax
    mov [rdi + 16], rax
    mov [rdi + 24], rax
    mov [rdi + 32], rax
    mov [rdi + 40], rcx    ; RDI = argument
    mov [rdi + 48], rax
    mov [rdi + 56], rdx    ; RSP = stack pointer
    mov [rdi + 64], rax
    mov [rdi + 72], rax
    mov [rdi + 80], rax
    mov [rdi + 88], rax
    mov [rdi + 96], rax
    mov [rdi + 104], rax
    mov [rdi + 112], rax
    mov [rdi + 120], rax

    mov [rdi + 128], rsi   ; RIP = entry point

    mov rax, 0x202         ; IF flag
    mov [rdi + 136], rax

    mov ax, 0x08           ; Kernel code segment
    mov [rdi + 144], ax
    mov ax, 0x10           ; Kernel data segment
    mov [rdi + 146], ax
    mov [rdi + 148], ax
    mov [rdi + 150], ax
    mov [rdi + 152], ax
    mov [rdi + 154], ax

    ret
