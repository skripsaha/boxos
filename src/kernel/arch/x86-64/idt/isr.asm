[BITS 64]

section .text

extern exception_handler
extern irq_handler

global isr_table

; ──────────────────────────────────────────────────────────────────────────────
; CET / IBT note (Intel SDM Vol 3D §17.3.4 — "Indirect Branch Tracking on
; Interrupt / Exception Delivery"):
;
; When CR4.CET=1 and IA32_S_CET.ENDBR_EN=1, delivery of an exception or
; interrupt sets the indirect-branch tracker to WAIT_FOR_ENDBRANCH. The
; very first instruction at the IDT-target MUST be ENDBR64, else #CP fires
; with error_code.endbranch_target=1 — the kernel would triple-fault on
; the very first interrupt after S_CET.ENDBR_EN flips on.
;
; Every ISR / IRQ stub below begins with ENDBR64. Outside CET/IBT, ENDBR64
; is a NOP (architectural — F3 0F 1E FA decoded as multi-byte NOP on pre-
; CET silicon). The `jmp isr_common` is a relative branch and is NOT an
; indirect transfer, so `isr_common` / `paranoid_isr_common` do not need
; their own ENDBR64.
; ──────────────────────────────────────────────────────────────────────────────

%macro ISR_NOERROR 1
isr%1:
    endbr64
    push 0          ; Dummy error code
    push %1         ; Interrupt vector
    jmp isr_common
%endmacro

%macro ISR_ERROR 1
isr%1:
    endbr64
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
    endbr64
    push 0          ; Dummy error code
    push %2         ; IRQ vector (32 + IRQ number)
    jmp isr_common
%endmacro

; --- Paranoid-entry variants for IST exceptions (#DB/#NMI/#DF/#SS/#MC) ---
;
; The standard isr_common decides "swap GS or not" from CS.RPL on the saved
; frame. For these five vectors that path is unsafe: SDM Vol 3A §6.14.5 hands
; the handler an IST stack regardless of source CPL, so the handler can
; nest INSIDE another interrupt's swapgs window. In that window, CS has
; already been pushed as kernel CS by the CPU's gate transition but the
; outer handler has not yet run its `swapgs` — active GS.base is still the
; user value. A CS.RPL check would then leave the handler running with the
; wrong GS.base.
;
; The race-free probe is to read IA32_GS_BASE via RDMSR and test the high
; half: kernel base is canonical-high (bit 63 set, EDX negative), user/zero
; is canonical-low (EDX non-negative). This is the same shape as the Linux
; `paranoid_entry` pattern. See SDM Vol 3A §3.4.3 (IA32_GS_BASE) and §6.7
; (NMI handling considerations).
%macro ISR_PARANOID_NOERROR 1
isr%1:
    endbr64
    push 0          ; Dummy error code
    push %1         ; Interrupt vector
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

ISR_NOERROR 0           ; Divide by zero
ISR_PARANOID_NOERROR 1  ; Debug                  — IST4
ISR_PARANOID_NOERROR 2  ; NMI                    — IST2
ISR_NOERROR 3           ; Breakpoint
ISR_NOERROR 4           ; Overflow
ISR_NOERROR 5           ; Bound range exceeded
ISR_NOERROR 6           ; Invalid opcode
ISR_NOERROR 7           ; Device not available
ISR_PARANOID_ERROR   8  ; Double fault           — IST1
ISR_NOERROR 9           ; Coprocessor segment overrun
ISR_ERROR   10          ; Invalid TSS
ISR_ERROR   11          ; Segment not present
ISR_PARANOID_ERROR   12 ; Stack fault            — IST5
ISR_ERROR   13          ; General protection fault
ISR_ERROR   14          ; Page fault
ISR_NOERROR 15          ; Reserved
ISR_NOERROR 16          ; FPU error
ISR_ERROR   17          ; Alignment check
ISR_PARANOID_NOERROR 18 ; Machine check          — IST3
ISR_NOERROR 19          ; SIMD exception
ISR_NOERROR 20  ; Virtualization exception (#VE)
ISR_ERROR   21  ; Control-Protection Exception (#CP) — Intel SDM Vol 3A
                ; Table 6-1: pushes a 15-bit type field + ENCL bit error
                ; code. Treating it as NOERROR shifts the entire frame
                ; by 8 bytes — every #CP would land on a corrupted frame
                ; and triple-fault. CR4.CET=1 enables this delivery; on
                ; QEMU TCG (no SHSTK/IBT) it stays dormant, on real silicon
                ; (Tiger Lake+ / Zen 3+) any ROP/JOP attempt fires #CP.
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

; Hardware interrupts (IRQ 0-15 -> vectors 32-47, PIC compatible)
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

; IO-APIC extra IRQs (GSI 16-23 -> vectors 48-55)
IRQ 16, 48      ; PCI / IO-APIC pin 16
IRQ 17, 49      ; PCI / IO-APIC pin 17
IRQ 18, 50      ; PCI / IO-APIC pin 18
IRQ 19, 51      ; PCI / IO-APIC pin 19
IRQ 20, 52      ; PCI / IO-APIC pin 20
IRQ 21, 53      ; PCI / IO-APIC pin 21
IRQ 22, 54      ; PCI / IO-APIC pin 22
IRQ 23, 55      ; PCI / IO-APIC pin 23

; System call (INT 0x80)
ISR_NOERROR 128  ; kernel_notify syscall

; Completion IRQ (INT 0x81)
ISR_NOERROR 129  ; workflow completion notification

; MSI vectors — message-signalled interrupts, delivered straight to the LAPIC
ISR_NOERROR 112  ; AHCI MSI  (0x70)
ISR_NOERROR 113  ; xHCI MSI  (0x71)

; LAPIC special vectors
ISR_NOERROR 254  ; LAPIC timer
ISR_NOERROR 255  ; LAPIC spurious

; IPI vectors (AMP inter-processor interrupts)
ISR_NOERROR 240  ; IPI_WAKE     (0xF0)
ISR_NOERROR 241  ; IPI_SHOOTDOWN (0xF1)
ISR_NOERROR 242  ; IPI_PANIC    (0xF2)

; Common ISR entry point
; Stack layout on entry (top to bottom):
;   r15, r14, r13, r12, r11, r10, r9, r8
;   rbp, rdi, rsi, rdx, rcx, rbx, rax
;   vector, error_code
;   rip, cs, rflags, rsp, ss  (saved by CPU)
isr_common:
    ; Conditional swapgs on entry: if we interrupted user mode (CS.RPL==3),
    ; the active GS base is the user value — swap to the per-cpu kernel base so
    ; C handlers can read %gs (amp_get_core_index etc.). If we interrupted
    ; kernel mode (nested IRQ/exception/IST), the active base is ALREADY the
    ; per-cpu base, so we must NOT swap. At this point rsp -> vector, and the
    ; CPU frame sits above it: vector(+0) err(+8) rip(+16) cs(+24) ...
    test byte [rsp+24], 3               ; came from user?
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

    ; Conditional swapgs on exit: mirror the entry. The handler may have
    ; rewritten the frame (scheduler picks the next task), so test the CURRENT
    ; frame CS. Returning to user (RPL==3) -> swap user GS base back in;
    ; returning to kernel (idle/kcore, RPL==0) -> keep the per-cpu base.
    ; After `add rsp,16` the stack top is the CPU frame: rip(+0) cs(+8) ...
    test byte [rsp+8], 3                ; returning to user?
    jz .isr_exit_kernel
    swapgs
.isr_exit_kernel:
    iretq

; =============================================================================
; paranoid_isr_common — entry pipeline for IST vectors (#DB/#NMI/#DF/#SS/#MC).
;
; Probes IA32_GS_BASE via RDMSR instead of trusting CS.RPL, so a nested IST
; that lands inside another interrupt's swapgs window still computes the right
; GS state. The swap decision made on entry is carried to exit through a
; dedicated stack slot pushed just below the standard interrupt_frame_t, then
; reversed before iretq so a nested IST cannot strand the outer interrupt with
; a flipped GS base. The C handler sees the same interrupt_frame_t* as the
; normal path; it never observes the smuggled flag.
;
; Spec refs:
;   Intel SDM Vol 3A §3.4.3   — IA32_GS_BASE (MSR 0xC0000101)
;   Intel SDM Vol 3A §6.7     — NMI handling; recommends paranoid GS probe
;   Intel SDM Vol 3A §6.14.5  — IST mechanism in IA-32e mode
; =============================================================================
paranoid_isr_common:
    ; Stack on entry:
    ;   rsp+0   vector
    ;   rsp+8   error_code
    ;   rsp+16  rip
    ;   rsp+24  cs
    ;   rsp+32  rflags
    ;   rsp+40  rsp
    ;   rsp+48  ss

    ; ---- Probe GS.base ----
    ; Save scratch regs (RDMSR uses RAX/RCX/RDX).
    push rax
    push rcx
    push rdx

    mov ecx, 0xC0000101     ; IA32_GS_BASE
    rdmsr                   ; EDX:EAX = current GS.base

    ; Kernel GS.base is canonical-high (bit 63 set → EDX < 0).
    ; User / zero base is canonical-low (EDX >= 0). Need swap iff base is low.
    xor eax, eax
    test edx, edx
    setns al                ; al = 1 if EDX non-negative (user/zero) → swap needed

    ; Smuggle the swap flag into the high 32 bits of the saved error_code
    ; (now at rsp+32 after 3 scratch pushes). The low 32 bits hold the actual
    ; error code (always fits in 32 bits for #DB/#NMI/#DF/#SS/#MC), the high
    ; half is currently zero in every case (dummy push from the NOERROR macro
    ; or zero pushed by the CPU for #DF). We restore the high half before the
    ; C handler sees the frame.
    mov [rsp + 32 + 4], eax

    test al, al
    jz .par_entry_keep_gs
    swapgs
.par_entry_keep_gs:

    pop rdx
    pop rcx
    pop rax

    ; ---- Build interrupt_frame_t (same layout as isr_common) ----
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

    ; Stack now: r15(+0) ... rax(+112) vector(+120) error_code(+128) rip(+136) ...

    ; Recover the swap flag from the smuggled high half of error_code, then
    ; zero the high half so the C handler sees a clean 32-bit error code.
    ; (Using `and qword, 0xFFFFFFFF` would sign-extend the 32-bit immediate
    ;  to 0xFFFFFFFFFFFFFFFF — a no-op.)
    mov eax, [rsp + 128 + 4]
    mov dword [rsp + 128 + 4], 0

    ; Push swap flag as a dedicated slot directly below the frame.
    ; After this push: [rsp+0] = swap_flag, [rsp+8] = r15, ...
    push rax

    lea rdi, [rsp + 8]      ; arg0 = interrupt_frame_t* (skip the flag slot)

    ; 16-byte align per SysV AMD64 ABI §3.2 before `call`. After 16 qword
    ; pushes (15 GPRs + flag) on top of an already-aligned CPU frame, we are
    ; aligned; the dynamic correction below is defensive and zero-cost.
    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax                ; save the alignment correction

    ; Every paranoid vector is an exception (1, 2, 8, 12, 18 — all < 32),
    ; so dispatch directly. No IRQ / syscall path lives here.
    call exception_handler

    pop rax
    add rsp, rax            ; undo alignment correction

    pop rax                 ; rax = swap_flag

    ; ---- Mirror the entry swap on exit, BEFORE restoring GPRs. ----
    ; This deliberately runs before pop r15..rax so the eventual register
    ; restore can clobber any temporary we used here. swapgs is a hardware
    ; instruction that affects MSR state only, not GPRs.
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

    add rsp, 16             ; drop vector + error_code

    iretq

section .data
isr_table:
    ; Exceptions (0-31)
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    ; IRQs (32-47): PIC / IO-APIC ISA IRQs
    dq irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dq irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    ; IRQs (48-55): IO-APIC extra pins (GSI 16-23)
    dq irq16, irq17, irq18, irq19, irq20, irq21, irq22, irq23
    ; Unimplemented (56-111) - use GPF handler
    times 56 dq isr13
    ; AHCI MSI (112 = 0x70)
    dq isr112
    ; xHCI MSI (113 = 0x71)
    ;
    ; A vector with no stub of its own is not an unused vector — every entry in
    ; the unimplemented runs below points at isr13, the General Protection
    ; Fault handler. So the first interrupt the xHCI controller delivered
    ; arrived as a #GP panic, and the panic named vector 13 rather than the
    ; vector that was actually raised. Adding a vector to irqchip.h and an
    ; IDT entry in idt.c is two thirds of the work; this is the third.
    dq isr113
    ; Unimplemented (114-127) - use GPF handler
    times 14 dq isr13
    ; Syscall (128)
    dq isr128
    ; Completion IRQ (129)
    dq isr129
    ; Unimplemented (130-239) - use GPF handler
    times 110 dq isr13
    ; IPI vectors (240-242)
    dq isr240
    dq isr241
    dq isr242
    ; Unimplemented (243-253) - use GPF handler
    times 11 dq isr13
    ; LAPIC timer (254)
    dq isr254
    ; LAPIC spurious (255)
    dq isr255
