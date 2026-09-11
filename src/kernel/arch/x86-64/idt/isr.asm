[BITS 64]

section .text

extern exception_handler
extern irq_handler

global isr_table


%macro ISR_NOERROR 1
isr%1:
    endbr64
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERROR 1
isr%1:
    endbr64
    xchg [rsp], rax
    push %1
    xchg [rsp+8], rax
    jmp isr_common
%endmacro

%macro IRQ 2
irq%1:
    endbr64
    push 0
    push %2
    jmp isr_common
%endmacro

%macro ISR_PARANOID_NOERROR 1
isr%1:
    endbr64
    push 0
    push %1
    jmp paranoid_isr_common
%endmacro

%macro ISR_PARANOID_ERROR 1
isr%1:
    endbr64
    xchg [rsp], rax
    push %1
    xchg [rsp+8], rax
    jmp paranoid_isr_common
%endmacro

ISR_NOERROR 0
ISR_PARANOID_NOERROR 1
ISR_PARANOID_NOERROR 2
ISR_NOERROR 3
ISR_NOERROR 4
ISR_NOERROR 5
ISR_NOERROR 6
ISR_NOERROR 7
ISR_PARANOID_ERROR   8
ISR_NOERROR 9
ISR_ERROR   10
ISR_ERROR   11
ISR_PARANOID_ERROR   12
ISR_ERROR   13
ISR_ERROR   14
ISR_NOERROR 15
ISR_NOERROR 16
ISR_ERROR   17
ISR_PARANOID_NOERROR 18
ISR_NOERROR 19
ISR_NOERROR 20
ISR_ERROR   21
ISR_NOERROR 22
ISR_NOERROR 23
ISR_NOERROR 24
ISR_NOERROR 25
ISR_NOERROR 26
ISR_NOERROR 27
ISR_NOERROR 28
ISR_NOERROR 29
ISR_ERROR   30
ISR_NOERROR 31

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

IRQ 16, 48
IRQ 17, 49
IRQ 18, 50
IRQ 19, 51
IRQ 20, 52
IRQ 21, 53
IRQ 22, 54
IRQ 23, 55

ISR_NOERROR 128

ISR_NOERROR 129

ISR_NOERROR 112
ISR_NOERROR 113
ISR_NOERROR 114
ISR_NOERROR 115
ISR_NOERROR 116
ISR_NOERROR 117
ISR_NOERROR 118
ISR_NOERROR 119
ISR_NOERROR 120

ISR_NOERROR 254
ISR_NOERROR 255

ISR_NOERROR 240
ISR_NOERROR 241
ISR_NOERROR 242

isr_common:
    test byte [rsp+24], 3
    jz .isr_entry_kernel
    swapgs
.isr_entry_kernel:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp

    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax

    mov rax, [rdi + 15*8]

    cmp rax, 128
    je .syscall

    cmp rax, 32
    jl .exception

    call irq_handler
    jmp .done

.exception:
    call exception_handler
    jmp .done

.syscall:
    extern syscall_handler
    call syscall_handler

.done:
    pop rax
    add rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16

    test byte [rsp+8], 3
    jz .isr_exit_kernel
    swapgs
.isr_exit_kernel:
    iretq

paranoid_isr_common:

    push rax
    push rcx
    push rdx

    mov ecx, 0xC0000101
    rdmsr

    xor eax, eax
    test edx, edx
    setns al

    mov [rsp + 32 + 4], eax

    test al, al
    jz .par_entry_keep_gs
    swapgs
.par_entry_keep_gs:

    pop rdx
    pop rcx
    pop rax

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15


    mov eax, [rsp + 128 + 4]
    mov dword [rsp + 128 + 4], 0

    push rax

    lea rdi, [rsp + 8]

    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax

    call exception_handler

    pop rax
    add rsp, rax

    pop rax

    test al, al
    jz .par_exit_keep_gs
    swapgs
.par_exit_keep_gs:

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16

    iretq

section .data
isr_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dq irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    dq irq16, irq17, irq18, irq19, irq20, irq21, irq22, irq23
    times 56 dq isr13
    dq isr112
    dq isr113
    dq isr114
    dq isr115
    dq isr116
    dq isr117
    dq isr118
    dq isr119
    dq isr120
    times 7 dq isr13
    dq isr128
    dq isr129
    times 110 dq isr13
    dq isr240
    dq isr241
    dq isr242
    times 11 dq isr13
    dq isr254
    dq isr255