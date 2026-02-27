[BITS 64]

; ============================================================================
; CONTEXT SWITCHING for BoxOS Tasks
; ============================================================================
; Functions:
;   - task_save_context(TaskContext* ctx)
;   - task_restore_context(TaskContext* ctx)
;   - task_switch_to(TaskContext* old_ctx, TaskContext* new_ctx)
; ============================================================================

section .text

; ============================================================================
; task_save_context - Save current CPU state to TaskContext
; ============================================================================
; Arguments: RDI = pointer to TaskContext structure
; Returns: nothing
; ============================================================================
global task_save_context
task_save_context:
    ; Save general purpose registers
    mov [rdi + 0],  rax
    mov [rdi + 8],  rbx
    mov [rdi + 16], rcx
    mov [rdi + 24], rdx
    mov [rdi + 32], rsi
    mov [rdi + 40], rdi
    mov [rdi + 48], rbp
    mov [rdi + 56], rsp    ; Stack pointer
    mov [rdi + 64], r8
    mov [rdi + 72], r9
    mov [rdi + 80], r10
    mov [rdi + 88], r11
    mov [rdi + 96], r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    ; Save RIP (return address from stack)
    mov rax, [rsp]
    mov [rdi + 128], rax

    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 136], rax

    ; Save segment registers
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

    ; FPU/SSE state is saved separately by caller if needed
    ret

; ============================================================================
; task_restore_context - Restore CPU state from TaskContext
; ============================================================================
; Arguments: RDI = pointer to TaskContext structure
; Returns: Does not return! Jumps to restored RIP
; ============================================================================
global task_restore_context
task_restore_context:
    ; Restore segment registers first
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

    ; Restore RFLAGS
    mov rax, [rdi + 136]
    push rax
    popfq

    ; Restore stack pointer (need to do this carefully)
    mov rsp, [rdi + 56]

    ; Restore general purpose registers (except RAX, RDI which we need)
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

    ; Push RIP onto stack for return
    mov rax, [rdi + 128]
    push rax

    ; Restore RAX and RDI last
    mov rax, [rdi + 0]
    mov rdi, [rdi + 40]

    ; Return to saved RIP
    ret

; ============================================================================
; task_switch_to - Switch from one task context to another
; ============================================================================
; Arguments: RDI = pointer to current TaskContext (to save)
;            RSI = pointer to next TaskContext (to restore)
; Returns: Does not return normally (switches context)
; ============================================================================
global task_switch_to
task_switch_to:
    ; Save current context
    ; Save general purpose registers
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

    ; Save RIP (return address)
    mov rax, [rsp]
    mov [rdi + 128], rax

    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 136], rax

    ; Save segments
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

    ; === Now restore new context from RSI ===

    ; Restore segments
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

    ; Restore RFLAGS
    mov rax, [rsi + 136]
    push rax
    popfq

    ; Restore stack pointer
    mov rsp, [rsi + 56]

    ; Restore general purpose registers
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

    ; Push RIP for return
    mov rax, [rsi + 128]
    push rax

    ; Restore RAX, RSI, RDI last
    mov rax, [rsi + 0]
    mov rdi, [rsi + 40]
    mov rsi, [rsi + 32]

    ; Jump to new task
    ret

; ============================================================================
; task_init_context - Initialize a new task context
; ============================================================================
; Arguments: RDI = pointer to TaskContext
;            RSI = entry point (RIP)
;            RDX = stack pointer (RSP)
;            RCX = argument (will be in RDI when task starts)
; ============================================================================
global task_init_context
task_init_context:
    ; Zero out all registers
    xor rax, rax
    mov [rdi + 0],  rax    ; RAX = 0
    mov [rdi + 8],  rax    ; RBX = 0
    mov [rdi + 16], rax    ; RCX = 0
    mov [rdi + 24], rax    ; RDX = 0
    mov [rdi + 32], rax    ; RSI = 0
    mov [rdi + 40], rcx    ; RDI = argument
    mov [rdi + 48], rax    ; RBP = 0
    mov [rdi + 56], rdx    ; RSP = stack pointer
    mov [rdi + 64], rax    ; R8-R15 = 0
    mov [rdi + 72], rax
    mov [rdi + 80], rax
    mov [rdi + 88], rax
    mov [rdi + 96], rax
    mov [rdi + 104], rax
    mov [rdi + 112], rax
    mov [rdi + 120], rax

    ; Set RIP to entry point
    mov [rdi + 128], rsi

    ; Set RFLAGS (enable interrupts)
    mov rax, 0x202         ; IF (interrupt enable) flag
    mov [rdi + 136], rax

    ; Set segments to kernel segments
    mov ax, 0x08           ; Kernel code segment
    mov [rdi + 144], ax
    mov ax, 0x10           ; Kernel data segment
    mov [rdi + 146], ax
    mov [rdi + 148], ax
    mov [rdi + 150], ax
    mov [rdi + 152], ax
    mov [rdi + 154], ax

    ret
