[BITS 64]

section .text

extern exception_handler
extern irq_handler

global isr_table

%macro ISR_NOERROR 1
isr%1:
    push 0          ; Dummy error code
    push %1         ; Interrupt vector
    jmp isr_common
%endmacro

%macro ISR_ERROR 1
isr%1:
    ; Error code already pushed by CPU, just add vector
    ; Stack now: error_code (from CPU)
    ; We need: vector, error_code
    xchg [rsp], rax      ; Save rax, get error_code in rax
    push %1              ; Push vector
    xchg [rsp+8], rax    ; Restore error_code position, get rax back
    jmp isr_common
%endmacro

%macro IRQ 2
irq%1:
    push 0          ; Dummy error code
    push %2         ; IRQ vector (32 + IRQ number)
    jmp isr_common
%endmacro

ISR_NOERROR 0   ; Divide by zero
ISR_NOERROR 1   ; Debug
ISR_NOERROR 2   ; NMI
ISR_NOERROR 3   ; Breakpoint
ISR_NOERROR 4   ; Overflow
ISR_NOERROR 5   ; Bound range exceeded
ISR_NOERROR 6   ; Invalid opcode
ISR_NOERROR 7   ; Device not available
ISR_ERROR   8   ; Double fault
ISR_NOERROR 9   ; Coprocessor segment overrun
ISR_ERROR   10  ; Invalid TSS
ISR_ERROR   11  ; Segment not present
ISR_ERROR   12  ; Stack fault
ISR_ERROR   13  ; General protection fault
ISR_ERROR   14  ; Page fault
ISR_NOERROR 15  ; Reserved
ISR_NOERROR 16  ; FPU error
ISR_ERROR   17  ; Alignment check
ISR_NOERROR 18  ; Machine check
ISR_NOERROR 19  ; SIMD exception
ISR_NOERROR 20  ; Virtualization exception
ISR_NOERROR 21  ; Reserved
ISR_NOERROR 22  ; Reserved
ISR_NOERROR 23  ; Reserved
ISR_NOERROR 24  ; Reserved
ISR_NOERROR 25  ; Reserved
ISR_NOERROR 26  ; Reserved
ISR_NOERROR 27  ; Reserved
ISR_NOERROR 28  ; Reserved
ISR_NOERROR 29  ; Reserved
ISR_ERROR   30  ; Security exception
ISR_NOERROR 31  ; Reserved

; Hardware interrupts (IRQ 0-15 -> vectors 32-47)
IRQ 0, 32       ; Timer
IRQ 1, 33       ; Keyboard
IRQ 2, 34       ; Cascade
IRQ 3, 35       ; COM2
IRQ 4, 36       ; COM1
IRQ 5, 37       ; LPT2
IRQ 6, 38       ; Floppy
IRQ 7, 39       ; LPT1
IRQ 8, 40       ; RTC
IRQ 9, 41       ; Free
IRQ 10, 42      ; Free
IRQ 11, 43      ; Free
IRQ 12, 44      ; Mouse
IRQ 13, 45      ; FPU
IRQ 14, 46      ; ATA Primary
IRQ 15, 47      ; ATA Secondary

; System call (INT 0x80)
ISR_NOERROR 128  ; kernel_notify syscall

; Completion IRQ (INT 0x81)
ISR_NOERROR 129  ; workflow completion notification

; Common ISR entry point
; Stack layout on entry (top to bottom):
;   r15, r14, r13, r12, r11, r10, r9, r8
;   rbp, rdi, rsi, rdx, rcx, rbx, rax
;   vector, error_code
;   rip, cs, rflags, rsp, ss  (saved by CPU)
isr_common:
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

    mov rdi, rsp    ; rdi = pointer to interrupt_frame_t

    ; Align stack to 16 bytes for System V ABI
    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax        ; Save alignment offset

    mov rax, [rdi + 15*8]   ; vector field offset

    ; Check for syscall (vector 128 = 0x80)
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
    add rsp, rax    ; Restore stack alignment

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

    add rsp, 16     ; Remove vector and error_code

    iretq

section .data
isr_table:
    ; Exceptions (0-31)
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    ; IRQs (32-47)
    dq irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dq irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    ; Unimplemented (48-127) - use GPF handler
    times 80 dq isr13
    ; Syscall (128)
    dq isr128
    ; Completion IRQ (129)
    dq isr129
    ; Unimplemented (130-255) - use GPF handler
    times 126 dq isr13
