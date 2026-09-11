
[BITS 16]

section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_data

%define DATA_OFFSET_CR3        (ap_trampoline_data - ap_trampoline_start)
%define DATA_OFFSET_STACK      (ap_trampoline_data - ap_trampoline_start + 8)
%define DATA_OFFSET_CORE_IDX   (ap_trampoline_data - ap_trampoline_start + 16)
%define DATA_OFFSET_EXTRA_CR4  (ap_trampoline_data - ap_trampoline_start + 20)
%define DATA_OFFSET_EXTRA_EFER (ap_trampoline_data - ap_trampoline_start + 24)
%define GDT_PTR_OFFSET         (ap_gdt_ptr - ap_trampoline_start)

ap_trampoline_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    o32 lgdt [0x8000 + GDT_PTR_OFFSET]

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
    or eax, (1 << 5)  | (1 << 7) | (1 << 9) | (1 << 10)
    mov ebx, 0x8000
    or eax, [ebx + DATA_OFFSET_EXTRA_CR4]
    mov cr4, eax

    mov ebx, 0x8000
    mov eax, [ebx + DATA_OFFSET_CR3]
    mov cr3, eax

    mov ebx, 0x8000
    mov esi, [ebx + DATA_OFFSET_EXTRA_EFER]
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    or eax, esi
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

    mov rbx, 0x8000
    mov rsp, [rbx + DATA_OFFSET_STACK]

    mov rsi, rsp
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
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00

ap_gdt_ptr:
    dw (ap_gdt_ptr - ap_gdt - 1)
    dd 0x8000 + (ap_gdt - ap_trampoline_start)

align 8
ap_trampoline_data:
    dq 0
    dq 0
    db 0
    db 0
    db 0
    db 0
    dd 0
    dd 0

ap_trampoline_end: