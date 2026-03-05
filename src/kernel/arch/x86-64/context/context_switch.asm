[BITS 64]

%include "gdt_selectors.inc"

section .text

;--------------------------------------------------------------
; Offsets into ProcessContext (must match C struct in process.h)
;
; GPRs:              0..127  (16 x uint64_t)
; rip:               128     (uint64_t)
; cs,ds,es,fs,gs,ss: 136..147  (6 x uint16_t)
; [4-byte padding]:  148..151  (alignment for uint64_t)
; rflags:            152     (uint64_t)
; cr3:               160     (uint64_t)
; fpu_state:         168     (uint8_t* pointer, 8 bytes)
; fpu_initialized:   176     (bool, 1 byte)
;--------------------------------------------------------------

%define CTX_RIP      128
%define CTX_CS       136
%define CTX_DS       138
%define CTX_ES       140
%define CTX_FS       142
%define CTX_GS       144
%define CTX_SS       146
%define CTX_RFLAGS   152
%define CTX_FPU      168
%define CTX_FPU_INIT 176

; Externals from fpu.c
extern g_use_xsave
extern g_xsave_mask

;--------------------------------------------------------------
; task_save_context(ProcessContext* ctx)
;   rdi = pointer to ProcessContext
;--------------------------------------------------------------
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
    mov [rdi + CTX_RIP], rax

    pushfq
    pop rax
    mov [rdi + CTX_RFLAGS], rax

    mov ax, cs
    mov [rdi + CTX_CS], ax
    mov ax, ds
    mov [rdi + CTX_DS], ax
    mov ax, es
    mov [rdi + CTX_ES], ax
    mov ax, fs
    mov [rdi + CTX_FS], ax
    mov ax, gs
    mov [rdi + CTX_GS], ax
    mov ax, ss
    mov [rdi + CTX_SS], ax

    ; Save FPU/SSE/AVX state
    ; Load pointer to FPU buffer and align to 64 bytes
    mov rcx, [rdi + CTX_FPU]
    test rcx, rcx
    jz .save_fpu_done
    add rcx, 63
    and rcx, -64

    cmp byte [rel g_use_xsave], 0
    je .save_fxsave

    ; xsave path: EDX:EAX = component mask, [rcx] = destination
    mov eax, dword [rel g_xsave_mask]
    mov edx, dword [rel g_xsave_mask + 4]
    xsave [rcx]
    jmp .save_fpu_mark

.save_fxsave:
    fxsave [rcx]

.save_fpu_mark:
    mov byte [rdi + CTX_FPU_INIT], 1

.save_fpu_done:
    ret

;--------------------------------------------------------------
; task_restore_context(ProcessContext* ctx)
;   rdi = pointer to ProcessContext
;--------------------------------------------------------------
global task_restore_context
task_restore_context:
    ; Restore FPU/SSE/AVX state first (uses rcx, rax, rdx as scratch)
    cmp byte [rdi + CTX_FPU_INIT], 0
    je .skip_fpu_restore

    mov rcx, [rdi + CTX_FPU]
    test rcx, rcx
    jz .skip_fpu_restore
    add rcx, 63
    and rcx, -64

    cmp byte [rel g_use_xsave], 0
    je .restore_fxsave

    mov eax, dword [rel g_xsave_mask]
    mov edx, dword [rel g_xsave_mask + 4]
    xrstor [rcx]
    jmp .skip_fpu_restore

.restore_fxsave:
    fxrstor [rcx]

.skip_fpu_restore:

    mov ax, [rdi + CTX_DS]
    mov ds, ax
    mov ax, [rdi + CTX_ES]
    mov es, ax
    mov ax, [rdi + CTX_FS]
    mov fs, ax
    mov ax, [rdi + CTX_GS]
    mov gs, ax
    mov ax, [rdi + CTX_SS]
    mov ss, ax

    mov rax, [rdi + CTX_RFLAGS]
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

    mov rax, [rdi + CTX_RIP]
    push rax

    ; Restore rax and rdi last
    mov rax, [rdi + 0]
    mov rdi, [rdi + 40]

    ret

;--------------------------------------------------------------
; task_switch_to(ProcessContext* old, ProcessContext* new)
;   rdi = old context, rsi = new context
;--------------------------------------------------------------
global task_switch_to
task_switch_to:
    ; --- Save old context ---
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
    mov [rdi + CTX_RIP], rax

    pushfq
    pop rax
    mov [rdi + CTX_RFLAGS], rax

    mov ax, cs
    mov [rdi + CTX_CS], ax
    mov ax, ds
    mov [rdi + CTX_DS], ax
    mov ax, es
    mov [rdi + CTX_ES], ax
    mov ax, fs
    mov [rdi + CTX_FS], ax
    mov ax, gs
    mov [rdi + CTX_GS], ax
    mov ax, ss
    mov [rdi + CTX_SS], ax

    ; Save old FPU/SSE/AVX state
    mov rcx, [rdi + CTX_FPU]
    test rcx, rcx
    jz .switch_save_done
    add rcx, 63
    and rcx, -64

    cmp byte [rel g_use_xsave], 0
    je .switch_save_fxsave

    mov eax, dword [rel g_xsave_mask]
    mov edx, dword [rel g_xsave_mask + 4]
    xsave [rcx]
    jmp .switch_save_mark

.switch_save_fxsave:
    fxsave [rcx]

.switch_save_mark:
    mov byte [rdi + CTX_FPU_INIT], 1

.switch_save_done:

    ; --- Restore new context ---
    ; Restore FPU/SSE/AVX state first
    cmp byte [rsi + CTX_FPU_INIT], 0
    je .switch_skip_fpu

    mov rcx, [rsi + CTX_FPU]
    test rcx, rcx
    jz .switch_skip_fpu
    add rcx, 63
    and rcx, -64

    cmp byte [rel g_use_xsave], 0
    je .switch_restore_fxsave

    mov eax, dword [rel g_xsave_mask]
    mov edx, dword [rel g_xsave_mask + 4]
    xrstor [rcx]
    jmp .switch_skip_fpu

.switch_restore_fxsave:
    fxrstor [rcx]

.switch_skip_fpu:

    mov ax, [rsi + CTX_DS]
    mov ds, ax
    mov ax, [rsi + CTX_ES]
    mov es, ax
    mov ax, [rsi + CTX_FS]
    mov fs, ax
    mov ax, [rsi + CTX_GS]
    mov gs, ax
    mov ax, [rsi + CTX_SS]
    mov ss, ax

    mov rax, [rsi + CTX_RFLAGS]
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

    mov rax, [rsi + CTX_RIP]
    push rax

    ; Restore rax, rsi, rdi last
    mov rax, [rsi + 0]
    mov rdi, [rsi + 40]
    mov rsi, [rsi + 32]

    ret

;--------------------------------------------------------------
; task_init_context(ProcessContext* ctx, void* entry, void* stack, void* arg)
;   rdi = context, rsi = entry point, rdx = stack pointer, rcx = argument
;--------------------------------------------------------------
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

    mov [rdi + CTX_RIP], rsi   ; RIP = entry point

    mov rax, 0x202             ; IF flag set
    mov [rdi + CTX_RFLAGS], rax

    mov ax, GDT_KERNEL_CODE
    mov [rdi + CTX_CS], ax
    mov ax, GDT_KERNEL_DATA
    mov [rdi + CTX_DS], ax
    mov [rdi + CTX_ES], ax
    mov [rdi + CTX_FS], ax
    mov [rdi + CTX_GS], ax
    mov [rdi + CTX_SS], ax

    ; FPU not initialized — caller should use fpu_init_state() from C
    mov byte [rdi + CTX_FPU_INIT], 0

    ret
