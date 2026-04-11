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

; Higher-half offset: VMA - LMA
KERNEL_VMA_OFFSET equ 0xFFFFFFFF80000000

_start:
    jmp short .past_header          ; 2 bytes — skip header
    db 'KERNEL'                     ; 6 bytes — magic identifier
    dd 1                            ; 4 bytes — header version
    dd _kernel_phys_end             ; 4 bytes — true kernel PHYSICAL end (includes BSS)
    times 16 db 0                   ; 16 bytes — reserved
.past_header:
    ; Debug: 'K' on serial — we're alive at identity address
    mov al, 'K'
    mov dx, 0x3f8
    out dx, al

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Read stack address from boot_info structure (identity address, boot tables active)
    ; boot_info at 0x9000, stack_base at offset +32
    xor rsp, rsp
    mov esp, [0x9000 + 32]
    mov rbp, rsp

    mov al, 'S'
    mov dx, 0x3f8
    out dx, al

    ; ---- Jump to higher-half ----
    ; RIP is at identity address (~0x10XXXX). LEA [rel] gives identity address.
    ; Add KERNEL_VMA_OFFSET to get higher-half address. Stage2 mapped both.
    lea rax, [rel .higher_half]
    mov rbx, KERNEL_VMA_OFFSET
    add rax, rbx
    jmp rax

.higher_half:
    ; Now executing at higher-half address (0xFFFFFFFF801XXXXX)
    mov al, 'H'
    mov dx, 0x3f8
    out dx, al

    ; Convert RSP from identity to higher-half
    ; Boot stack is at physical ~0x3XXXXX, mapped by PD_high in stage2
    mov rax, KERNEL_VMA_OFFSET
    add rsp, rax
    add rbp, rax

    mov al, 'M'
    mov dx, 0x3f8
    out dx, al

    ; Zero BSS — C standard requires uninitialized globals be zero.
    ; Linker resolves __bss_start/__bss_end to higher-half VMA addresses.
    ; Page tables map these to the correct physical memory.
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

    call kernel_main

    mov al, 'R'
    mov dx, 0x3f8
    out dx, al

    cli
    hlt
    jmp $

write_port:
    mov dx, di
    mov al, sil
    out dx, al
    ret

; RDI = port, returns RAX = byte read
read_port:
    mov dx, di
    in al, dx
    movzx eax, al
    ret

; Returns RAX = GDT base address
get_gdt_base:
    sub rsp, 16
    sgdt [rsp]
    mov rax, [rsp + 2]  ; Skip limit (2 bytes), get base
    add rsp, 16
    ret

; RDI = pointer to GDT descriptor
load_gdt:
    lgdt [rdi]

    mov ax, 0x20        ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x18           ; Kernel code segment
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

; RDI = string pointer, AH = color, RCX = screen offset
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
