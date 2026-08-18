; boxcxx — <csetjmp>: setjmp / longjmp for x86-64 SysV, under CET.
;
; jmp_buf layout (9 x uint64, must match <csetjmp>):
;   0 rbx   1 rbp   2 r12   3 r13   4 r14   5 r15
;   6 rsp   — as the CALLER will see it, i.e. rsp+8 at entry
;   7 rip   — setjmp's own return address: the point execution resumes at
;   8 ssp   — the shadow-stack pointer at the moment setjmp ran, or 0
;
; ‼ The CET part is the whole difficulty, and it is why longjmp ends in a RET
; rather than an indirect jump.
;
; A longjmp travels UP the stack, so by the time it runs the shadow stack holds
; one entry for every frame the jump is about to discard. Leaving them there
; would fault the first time the resumed function returned — the return address
; on the normal stack would not match the one on the shadow stack, and the CPU
; would raise #CP. So they are popped with INCSSPQ until SSP is back where
; setjmp saw it.
;
; At that point the top shadow entry IS setjmp's own return address, which is
; exactly where we want to go. Putting the same address on the normal stack and
; executing RET therefore consumes both stacks in step and lands where setjmp
; would have returned — the hardware's own model of "return from setjmp, again".
; An indirect JMP would leave the shadow entry behind AND require an ENDBR64 at
; a target that has none (IBT), which is why the unwinder's NOTRACK jump is
; right there and wrong here: it lands on a DWARF landing pad, this lands in the
; middle of an ordinary function.
;
; RDSSPQ is a NOP when shadow stacks are off (QEMU/TCG today), leaving 0 — the
; whole block is then skipped and the RET works for the ordinary reason. On real
; hardware with CET, it does the work above. Same code, both worlds.

[BITS 64]

section .text

%define JB_RBX 0*8
%define JB_RBP 1*8
%define JB_R12 2*8
%define JB_R13 3*8
%define JB_R14 4*8
%define JB_R15 5*8
%define JB_RSP 6*8
%define JB_RIP 7*8
%define JB_SSP 8*8

global __boxcxx_setjmp
global __boxcxx_longjmp

; int __boxcxx_setjmp(jmp_buf env)   — rdi = env
__boxcxx_setjmp:
    endbr64
    mov     [rdi + JB_RBX], rbx
    mov     [rdi + JB_RBP], rbp
    mov     [rdi + JB_R12], r12
    mov     [rdi + JB_R13], r13
    mov     [rdi + JB_R14], r14
    mov     [rdi + JB_R15], r15

    lea     rax, [rsp + 8]              ; rsp the caller will have after our ret
    mov     [rdi + JB_RSP], rax
    mov     rax, [rsp]                  ; our return address = the resume point
    mov     [rdi + JB_RIP], rax

    xor     rax, rax
    rdsspq  rax                         ; NOP → 0 when shadow stacks are off
    mov     [rdi + JB_SSP], rax

    xor     eax, eax                    ; the first return is always 0
    ret

; [[noreturn]] void __boxcxx_longjmp(jmp_buf env, int val)  — rdi = env, esi = val
__boxcxx_longjmp:
    endbr64
    mov     eax, esi
    test    eax, eax
    jnz     .value_ok
    mov     eax, 1                      ; longjmp(env, 0) makes setjmp return 1
.value_ok:

    ; ── shadow stack: pop the frames this jump discards ──────────────────
    mov     r8, [rdi + JB_SSP]          ; where setjmp stood
    test    r8, r8
    jz      .no_shstk                   ; setjmp ran without shadow stacks
    xor     r9, r9
    rdsspq  r9                          ; where we stand now
    test    r9, r9
    jz      .no_shstk
    mov     r10, 1
.pop_one:
    cmp     r9, r8
    jae     .no_shstk                   ; at (or above) the saved point
    incsspq r10                         ; one entry
    add     r9, 8
    jmp     .pop_one

.no_shstk:
    mov     rbx, [rdi + JB_RBX]
    mov     rbp, [rdi + JB_RBP]
    mov     r12, [rdi + JB_R12]
    mov     r13, [rdi + JB_R13]
    mov     r14, [rdi + JB_R14]
    mov     r15, [rdi + JB_R15]

    mov     rcx, [rdi + JB_RIP]         ; read BEFORE rsp moves (rdi stays valid)
    mov     rsp, [rdi + JB_RSP]
    sub     rsp, 8                      ; the slot setjmp's return address sat in
    mov     [rsp], rcx
    ret                                 ; consumes normal AND shadow stack
