
%define CR0_PG       (1 << 31)
%define CR4_LA57     (1 << 12)
%define KERNEL_VMA   0xFFFFFFFF80000000
%define DANCE_CS32   0x08
%define DANCE_CS64   0x10
%define DANCE_DS     0x18
%define KERNEL_CODE  0x08
%define KERNEL_DATA  0x10

[BITS 64]
section .text

global vmm_la57_runtime_enable

vmm_la57_runtime_enable:
    endbr64
    pushfq
    cli
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov rax, rdi
    shr rax, 32
    test rax, rax
    jnz .bail

    mov r15, KERNEL_VMA

    lea rax, [rel vmm_la57_saved_gdtr]
    sub rax, r15
    sgdt [rax]
    lea rax, [rel vmm_la57_saved_idtr]
    sub rax, r15
    sidt [rax]

    lea rax, [rel vmm_la57_dance_gdtr]
    sub rax, r15
    lgdt [rax]

    mov ebx, edi

    lea rax, [rel .low32]
    sub rax, r15
    lea rcx, [rel vmm_la57_jmp32]
    sub rcx, r15
    mov dword [rcx], eax
    mov word  [rcx + 4], DANCE_CS32

    lea rax, [rel .low_64_trampoline]
    sub rax, r15
    lea rcx, [rel vmm_la57_jmp64]
    sub rcx, r15
    mov dword [rcx], eax
    mov word  [rcx + 4], DANCE_CS64

    lea rax, [rel vmm_la57_jmp64]
    sub rax, r15
    mov ebp, eax

    lea r14, [rel vmm_la57_saved_gdtr]
    sub r14, r15

    lea rax, [rel vmm_la57_jmp32]
    sub rax, r15
    jmp far dword [rax]

[BITS 32]
.low32:
    mov ax, DANCE_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr0
    and eax, ~CR0_PG
    mov cr0, eax

    mov eax, cr4
    or  eax, CR4_LA57
    mov cr4, eax

    mov cr3, ebx

    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax

    jmp far dword [ebp]

[BITS 64]
.low_64_trampoline:
    mov rax, .high64
    jmp rax

.high64:
    lgdt [r14]

    mov ax, KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax

    sub rsp, 16
    lea rax, [rel .reload_cs]
    mov [rsp], rax
    mov qword [rsp + 8], KERNEL_CODE
    retfq

.reload_cs:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

.bail:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

section .data
align 8

global vmm_la57_dance_gdt
vmm_la57_dance_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
vmm_la57_dance_gdt_end:

global vmm_la57_dance_gdtr
vmm_la57_dance_gdtr:
    dw vmm_la57_dance_gdt_end - vmm_la57_dance_gdt - 1
    dq vmm_la57_dance_gdt - KERNEL_VMA

section .bss
align 8

global vmm_la57_saved_gdtr
vmm_la57_saved_gdtr:
    resb 16

global vmm_la57_saved_idtr
vmm_la57_saved_idtr:
    resb 16

global vmm_la57_jmp32
vmm_la57_jmp32:
    resb 8

global vmm_la57_jmp64
vmm_la57_jmp64:
    resb 8