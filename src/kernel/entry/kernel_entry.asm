[BITS 64]
DEFAULT ABS
[EXTERN kernel_main]

section .text
global _start

global write_port
global read_port

global get_gdt_base
global load_gdt

global clear_screen_vga
global hide_cursor

extern _kernel_phys_end

KERNEL_VMA_OFFSET equ 0xFFFFFFFF80000000

_start:
    jmp short .past_header
    db 'KERNEL'
    dd 1
    dd _kernel_phys_end
    times 16 db 0
.past_header:
    pushfq
    pop r15
    cli

    mov al, 'K'
    mov dx, 0x3f8
    out dx, al

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

%ifndef BOOT_INFO_ADDR
  %error "BOOT_INFO_ADDR must come from the build (-DBOOT_INFO_ADDR=...)"
%endif
    xor rsp, rsp
    mov esp, [BOOT_INFO_ADDR + 32]
    mov rbp, rsp

    mov al, 'S'
    mov dx, 0x3f8
    out dx, al

    lea rax, [rel .higher_half]
    mov rbx, KERNEL_VMA_OFFSET
    add rax, rbx
    jmp rax

.higher_half:
    mov al, 'H'
    mov dx, 0x3f8
    out dx, al

    mov rax, KERNEL_VMA_OFFSET
    add rsp, rax
    add rbp, rax

    mov al, 'M'
    mov dx, 0x3f8
    out dx, al

    extern __bss_start
    extern __bss_end

    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor rax, rax
    rep stosb

    mov al, 'Z'
    mov dx, 0x3f8
    out dx, al

    extern g_entry_rflags
    mov [g_entry_rflags], r15

    call kernel_main

    mov al, 'R'
    mov dx, 0x3f8
    out dx, al

    cli
    hlt
    jmp $

write_port:
    endbr64
    mov dx, di
    mov al, sil
    out dx, al
    ret

read_port:
    endbr64
    mov dx, di
    in al, dx
    movzx eax, al
    ret

get_gdt_base:
    endbr64
    sub rsp, 16
    sgdt [rsp]
    mov rax, [rsp + 2]
    add rsp, 16
    ret

load_gdt:
    endbr64
    lgdt [rdi]

    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x18
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

print_string_vga:
    push rdi
    push rbx
    push rax
    push rcx

    mov rbx, 0xB8000
    add rbx, rcx

.print_loop:
    mov al, [rdi]
    test al, al
    jz .done

    mov [rbx], ax
    add rbx, 2
    inc rdi
    jmp .print_loop

.done:
    pop rcx
    pop rax
    pop rbx
    pop rdi
    ret

clear_screen_vga:
    endbr64
    push rdi
    push rcx
    push rax

    mov rdi, 0xB8000
    mov rcx, 2000
    mov al, ' '
    mov ah, 0x07
    rep stosw

    pop rax
    pop rcx
    pop rdi
    ret

hide_cursor:
    endbr64
    mov dx, 0x3D4
    mov al, 0x0E
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al

    mov dx, 0x3D4
    mov al, 0x0F
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al
    ret