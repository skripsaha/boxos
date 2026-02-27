; ============================================================================
; BOXLIB - User Program Entry Point
; ============================================================================
; This is the entry point for all BoxOS user programs.
; The kernel jumps here after loading the ELF binary.
;
; On entry:
;   - Stack is set up by kernel
;   - Ring buffers are mapped at USER_RINGS_VADDR
;   - argc in RDI, argv in RSI (future)
;
; This file calls main() defined by the user program.
; ============================================================================

[BITS 64]

section .text

; External: user's main function
extern main

; Export: entry point
global _start

; ============================================================================
; _start - Program Entry Point
; ============================================================================
_start:
    ; Clear base pointer for stack traces
    xor rbp, rbp

    ; Align stack to 16 bytes (ABI requirement)
    and rsp, -16

    ; For now, argc = 0, argv = NULL
    ; TODO: Parse command line from kernel
    xor rdi, rdi        ; argc = 0
    xor rsi, rsi        ; argv = NULL

    ; Call user's main function
    ; int main(int argc, char** argv)
    call main

    ; main returned - exit with return code
    ; RAX contains return value from main
    mov rdi, rax        ; exit code = return value
    mov rsi, rax        ; also in RSI for kernel

    ; Call box_exit (NOTIFY_EXIT = 0x10)
    mov rdi, 0x10       ; NOTIFY_EXIT flag
    int 0x80            ; kernel_notify syscall

    ; Should never reach here, but just in case...
.halt:
    hlt
    jmp .halt

; ============================================================================
; Helper: Direct exit syscall (backup)
; ============================================================================
global box_exit_asm
box_exit_asm:
    ; RDI = exit code
    mov rsi, rdi        ; copy to RSI
    mov rdi, 0x10       ; NOTIFY_EXIT
    int 0x80
.halt:
    hlt
    jmp .halt
