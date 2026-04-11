; AP trampoline - copied to physical 0x8000 before SIPI
; Transitions AP: real mode -> protected mode -> long mode -> kernel
; No ORG directive - all absolute references use 0x8000 + (label - ap_trampoline_start)

[BITS 16]

section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_data

; Offsets of data fields relative to ap_trampoline_start
; Used in both 32-bit and 64-bit mode to avoid absolute-address NASM warnings
; by doing arithmetic in registers instead.
%define DATA_OFFSET_CR3        (ap_trampoline_data - ap_trampoline_start)
%define DATA_OFFSET_STACK      (ap_trampoline_data - ap_trampoline_start + 8)
%define DATA_OFFSET_CORE_IDX   (ap_trampoline_data - ap_trampoline_start + 16)
%define GDT_PTR_OFFSET         (ap_gdt_ptr - ap_trampoline_start)

ap_trampoline_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; lgdt needs absolute address of ap_gdt_ptr in physical memory
    lgdt [0x8000 + GDT_PTR_OFFSET]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:(0x8000 + (ap_protected - ap_trampoline_start))

[BITS 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load CR3 from data area using register arithmetic (avoids ABS warning)
    mov ebx, 0x8000
    mov eax, [ebx + DATA_OFFSET_CR3]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    or eax, (1 << 11)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    or eax, (1 << 16)
    mov cr0, eax

    jmp 0x18:(0x8000 + (ap_long_mode - ap_trampoline_start))

[BITS 64]
ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Use rbx as base = 0x8000 to avoid absolute-address deprecation warnings
    mov rbx, 0x8000
    mov rsp, [rbx + DATA_OFFSET_STACK]

    ; ap_entry_c(core_index, stack_top)
    ;   RDI = core_index  (first arg)
    ;   RSI = stack_top    (second arg)
    mov rsi, rsp                            ; RSI = stack top (before call pushes retaddr)
    xor rdi, rdi
    mov dil, [rbx + DATA_OFFSET_CORE_IDX]

    extern ap_entry_c
    mov rax, ap_entry_c
    call rax

.halt:
    cli
    hlt
    jmp .halt

align 16
ap_gdt:
    dq 0
    ; 0x08: 32-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 0x10: data (32 and 64-bit)
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
    ; 0x18: 64-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00

ap_gdt_ptr:
    dw (ap_gdt_ptr - ap_gdt - 1)
    dd 0x8000 + (ap_gdt - ap_trampoline_start)

align 8
ap_trampoline_data:
    dq 0    ; +0:  CR3 (physical PML4 address)
    dq 0    ; +8:  AP stack top (virtual)
    db 0    ; +16: core_index

ap_trampoline_end:
