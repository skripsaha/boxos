; userspace.asm - Ring 0 to Ring 3 transition

[BITS 64]

%include "gdt_selectors.inc"

global jump_to_userspace

; void jump_to_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);
; RDI = user RIP (entry point)
; RSI = user RSP (stack pointer)
; RDX = user RFLAGS
;
; Builds the iretq frame on the CURRENT (kernel) stack and lets iretq pop
; it + atomically switch SS:RSP to the user values. Writing the frame to
; the kernel stack is mandatory under CR4.SMAP=1 — any push to a user
; page while CPL=0 (which the previous "switch RSP first, then push"
; pattern did) triggers a #PF, which escalates to #DF on the next IST
; entry. SMAP is enabled under STRICT (`-cpu max` advertises it) so this
; bug only surfaced once stress-matrix STRICT configs actually applied
; their `-cpu max` flag — anywhere off STRICT (qemu64 default lacks
; SMAP) the broken path appeared to work by luck.
;
; Stack layout for iretq (Intel SDM Vol 2 §6.14.4 + §6.15 / IRET):
;       [SS]    <- rsp+32
;       [RSP]   <- rsp+24
;       [RFLAGS]<- rsp+16
;       [CS]    <- rsp+8
;       [RIP]   <- rsp
; The pushes below go in reverse so iretq pops them in this order.
;
; ABI: user code arrives with 16-byte aligned RSP (no return address
; pushed by iretq itself). The caller is responsible for handing in a
; 16-byte aligned user RSP — anything else is a caller bug.
jump_to_userspace:
    ; CET / IBT: declared `extern` in C and may be called via address
    ; in future scheduler hooks. ENDBR64 keeps the symbol IBT-safe.
    ; NOP without CR4.CET=1; required when S_CET.ENDBR_EN=1 if anyone
    ; ever reaches us via an indirect branch.
    endbr64
    cli

    mov ax, GDT_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax

    ; Crossing ring 0 -> ring 3 (single-core initial launch). The kernel runs
    ; with the per-cpu PerCpuData in the active GS base; swapgs parks it in the
    ; shadow (IA32_KERNEL_GS_BASE) so the process's first SYSCALL swaps it back.
    ; The following `mov gs,ax` sets the user GS selector; its descriptor base
    ; is 0, which is exactly the user-mode active GS base we want. The shadow
    ; (per-cpu) is untouched by the selector load.
    swapgs
    mov gs, ax

    ; Build iretq frame on the kernel stack (SMAP-safe).
    push GDT_USER_DATA   ; SS
    push rsi             ; user RSP
    push rdx             ; RFLAGS
    push GDT_USER_CODE   ; CS
    push rdi             ; user RIP

    iretq
