; Entry point for all BoxOS user programs.
; Kernel jumps here after loading the ELF binary.
;
; On entry:
;   - Stack is set up by kernel; per System V AMD64 psABI §3.2 we make RSP
;     16-byte aligned (mod 16 == 0) so the `call main` below leaves RSP%16==8
;     on main's first instruction, exactly what GCC's prologue assumes for
;     SSE-using callees.
;   - PocketRing header  at CABIN_POCKET_RING_ADDR (0x2000), slots lazy-mapped
;     at CABIN_POCKET_SLOTS_BASE (64 TiB).
;   - ResultRing header  at CABIN_RESULT_RING_ADDR (0x3000), slots lazy-mapped
;     at CABIN_RESULT_SLOTS_BASE.
;   - CabinInfo (PID, heap layout, stack_top, spawner_pid) at CABIN_INFO_ADDR
;     (0x1000), already populated by process.c::process_load.
;
; BoxOS user programs do NOT take argv via the entry point — argv is delivered
; over IPC via send_args()/receive_args() so the spawner can supply it as
; structured data rather than packed strings on the stack. We pass argc=0 /
; argv=NULL here for source-compat with C main() signatures.

[BITS 64]

section .text

extern main
extern exit

global _start

; CET / IBT note:
; The kernel reaches _start via IRETQ from ring 0, a cross-privilege return
; that initializes the user-mode indirect-branch tracker per
; IA32_U_CET.ENDBR_EN. With U_CET policy = SH_STK_EN|ENDBR_EN|NO_TRACK_EN
; (cet_lifecycle.c), the tracker arrives in WAIT_FOR_ENDBRANCH on the very
; first instruction at _start. Without ENDBR64 here, the process would #CP
; before main() is reached. ENDBR64 is a NOP without CR4.CET=1, so the
; non-CET path is unaffected.
;
; The same reasoning applies to any future userspace entry point (signal
; trampoline, async-notify handler) — every cross-privilege landing pad
; MUST begin with ENDBR64.
_start:
    endbr64
    xor rbp, rbp        ; clear base pointer so any stack trace stops here
    and rsp, -16        ; align RSP for the System V AMD64 ABI

    xor rdi, rdi        ; argc = 0   (see header comment)
    xor rsi, rsi        ; argv = NULL

    call main

    ; main returned — translate its return value into the BoxOS exit syscall.
    ; exit() in system.c sends the spawner a death notification (if any) and
    ; submits DECK_SYSTEM/SYS_PROC_KILL; it does not return.
    mov edi, eax
    call exit

    ; Defensive halt: if exit() ever returns (kernel torn down, etc.) park the
    ; process so we don't fall through into whatever follows in memory.
.halt:
    hlt
    jmp .halt
