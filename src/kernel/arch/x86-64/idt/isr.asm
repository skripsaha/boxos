[BITS 64]

; IA32_GS_BASE — the active GS base, the register RDGSBASE/WRGSBASE reach when
; CR4.FSGSBASE is on. RDMSR/WRMSR reach it on every long-mode CPU.
MSR_IA32_GS_BASE equ 0xC0000101

section .text

extern exception_handler
extern irq_handler
; Set with RELEASE at the bottom of per_core_init_bsp, i.e. after that core's
; LTR. Read here as "this core's task register names a PerCoreData TSS" — see
; the block above paranoid_isr_common.
extern g_per_core_active
; const uint64_t in per_core.c = offsetof(PerCoreData, tss). A symbol, not a
; build-time -D: the struct is the only place that number is written, so this
; subtrahend cannot drift away from the layout it belongs to.
extern PER_CORE_TSS_OFFSET

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
; SDM Vol 3A §6.14.5 hands these five vectors an IST stack regardless of the
; interrupted CPL, so they arrive from ring 3 as readily as from ring 0 — and
; the two origins want opposite handling. paranoid_isr_common splits them; the
; block in front of it says why, and how a ring-0 entry finds this core's
; true kernel GS base without asking GS.
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
ISR_NOERROR 113  ; xHCI MSI  (0x71) — first USB host controller
ISR_NOERROR 114  ; xHCI MSI  (0x72)
ISR_NOERROR 115  ; xHCI MSI  (0x73)
ISR_NOERROR 116  ; xHCI MSI  (0x74)
ISR_NOERROR 117  ; xHCI MSI  (0x75)
ISR_NOERROR 118  ; xHCI MSI  (0x76)
ISR_NOERROR 119  ; xHCI MSI  (0x77)
ISR_NOERROR 120  ; xHCI MSI  (0x78) — eighth, and the last this kernel brings up

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
; An IST vector taken from ring 3 is not a paranoid case at all. Ring 3 may
; write its own GS base — WRGSBASE lists no privilege condition (#UD only for
; the LOCK prefix, CR4.FSGSBASE=0 or the missing CPUID bit), and fpu.c sets
; CR4.FSGSBASE on every core that enumerates it — so the base itself proves
; nothing. But ring 3 cannot touch the swapgs shadow: IA32_KERNEL_GS_BASE is
; reachable only through WRMSR, at CPL 0. By the invariant in notify.h that
; shadow holds this core's per-cpu base for as long as a ring-3 context runs,
; so for those entries CS.RPL is the whole truth and swapgs is right at both
; ends. Hand them to isr_common, which tests the CURRENT frame's CS at entry
; AND at exit — which is what matters, because exception_handler reschedules
; for exactly these vectors and context_restore_to_frame rewrites frame->cs,
; so the ring returned to is not always the ring that was interrupted. Linux
; routes user-mode #DB/#MC away from its paranoid path the same way:
; idtentry_mce_db does `testb $3, CS-ORIG_RAX(%rsp); jnz ...switch_stack` into
; the noist body (arch/x86/entry/entry_64.S).
;
; From ring 0, CS.RPL is worthless: the CPU stamps a kernel CS the instant the
; gate is taken, so an IST vector landing inside another stub's swapgs window
; sees RPL 0 while the active base is still the ring-3 one. And no test on the
; BASE can separate the two either — a process that writes a canonical-high
; value with WRGSBASE reads exactly like the kernel. So this path tests
; nothing about GS. It asks the PROCESSOR where it is:
;
;   STR   this core's task-register selector. Ring 3 cannot write it — LTR is
;         a CPL-0 instruction and no ring-3 path reloads TR — which is what
;         makes it an anchor where the GS base is not one.
;   SGDT  the GDT that selector indexes. Also a register, not memory.
;
; The selector names a 64-bit TSS descriptor whose base is &g_per_core[i].tss;
; PER_CORE_TSS_OFFSET turns that into &g_per_core[i], which IS this core's
; kernel GS base. Nothing on the way is loaded through an address ring 3 can
; write: TR and GDTR come out of the CPU, the GDT they name is per-core kernel
; memory, and PER_CORE_TSS_OFFSET is kernel .rodata.
;
; With the true base in hand the entry stops deciding anything: it saves the
; base as found and installs the kernel one UNCONDITIONALLY, and the exit
; replays a fact instead of repeating a verdict — Linux's FSGSBASE
; paranoid_entry / paranoid_exit pair.
;
; Not RDGSBASE/WRGSBASE, and not RDPID:
;   - RDGSBASE/WRGSBASE #UD unless CR4.FSGSBASE=1, and fpu.c sets that bit only
;     on silicon that enumerates the feature (g_fsgsbase_active mirrors it), so
;     a pre-Ivy-Bridge board or a QEMU model without FSGSBASE would #UD on its
;     first NMI. IA32_GS_BASE reached by RDMSR/WRMSR is the same architectural
;     register and exists on every long-mode CPU. These five vectors are rare;
;     the MSR cost buys a path with no feature gate in it.
;   - Linux derives the per-cpu base with RDPID out of TSC_AUX
;     (GET_PERCPU_BASE). BoxOS never writes TSC_AUX, so that road is closed
;     here. Do not reopen it without giving TSC_AUX an owner first.
;
; Before g_per_core_active the anchor does not exist yet: the BSP still runs on
; the static GDT/TSS of gdt.c/tss.c, whose base is not inside a PerCoreData at
; all, and subtracting PER_CORE_TSS_OFFSET from it would name memory that is
; not per-cpu data. In that window the entry leaves GS alone — and that is not
; a hole, because no ring-3 context has ever run yet: there is no forged base
; to mistake for the kernel's and no swapgs window to land in, so the active
; base is the kernel's by construction. An AP cannot reach this code before its
; own LTR either: every vector routed here has a non-zero IST in idt_init, and
; the CPU cannot read an IST pointer through a null task register.
;
; The two facts the exit needs — the base as found, and this core's kernel base
; — ride in two stack slots pushed just below the standard interrupt_frame_t.
; The C handler sees the same interrupt_frame_t* as the normal path and never
; observes them.
;
; Spec refs:
;   Intel SDM Vol 2B "SWAPGS"            — exchanges GS base with MSR C0000102H
;   Intel SDM Vol 2  "WRFSBASE/WRGSBASE" — exception list; no CPL condition
;   Intel SDM Vol 2B "STR" / "SGDT"      — task register / GDTR read back
;   Intel SDM Vol 3A §8.2.3 Figure 8-4   — 64-bit TSS descriptor: base 15:0 at
;                                          byte 2, base 23:16 at byte 4, base
;                                          31:24 at byte 7, base 63:32 at bytes
;                                          8..11 (the same field order
;                                          per_core_gdt_set_tss writes)
;   Intel SDM Vol 3A §6.14.5             — IST delivery, whatever the CPL
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

    ; ---- Ring-3 entries leave here ----
    ; They are decided by CS.RPL at both ends, and isr_common does exactly
    ; that. Its stack layout is the one we already have.
    test byte [rsp + 24], 3
    jnz isr_common

    ; ---- Build interrupt_frame_t (same layout as isr_common) ----
    ; Nothing below reads %gs, so the GS repair waits until the general
    ; registers are saved and there is somewhere to work.
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

    ; Stack now: r15(+0) ... rax(+112) vector(+120) error_code(+128) rip(+136)
    ;            cs(+144)

    xor ebx, ebx                    ; rbx = this core's kernel GS base,
                                    ;       0 = "GS was left as found"
    xor esi, esi                    ; rsi = GS base as found
    cmp byte [rel g_per_core_active], 0
    je .par_gs_settled

    ; ---- This core's kernel GS base, out of the CPU's own registers ----
    str ax                          ; ax = task-register selector
    test ax, ax
    jz .par_gs_settled              ; no task loaded: there is no TSS base to
                                    ; anchor on, and a null selector would name
                                    ; the null descriptor. Unreachable in
                                    ; practice — every vector routed here has a
                                    ; non-zero IST and the CPU cannot read an
                                    ; IST pointer through a null TR — but the
                                    ; check costs two bytes and turns that
                                    ; argument into a fact.
    movzx eax, ax
    and eax, 0xFFF8                 ; byte offset of the descriptor in the GDT
    sub rsp, 16                     ; SGDT stores 10 bytes; 16 keeps rsp even
    sgdt [rsp]                      ; [rsp] = limit, [rsp+2] = GDT base
    add rax, [rsp + 2]              ; rax = &TSS descriptor
    add rsp, 16

    movzx ecx, word [rax + 2]       ; base 15:0
    movzx edx, byte [rax + 4]       ; base 23:16
    shl edx, 16
    or ecx, edx
    movzx edx, byte [rax + 7]       ; base 31:24
    shl edx, 24
    or ecx, edx
    mov edx, [rax + 8]              ; base 63:32
    shl rdx, 32
    or rcx, rdx                     ; rcx = &g_per_core[i].tss
    sub rcx, [rel PER_CORE_TSS_OFFSET]  ; rcx = &g_per_core[i] = kernel GS base
    mov rbx, rcx

    ; ---- Save the base as found, install the kernel one unconditionally ----
    mov ecx, MSR_IA32_GS_BASE
    rdmsr                           ; EDX:EAX = GS base at the moment of entry
    shl rdx, 32
    or rax, rdx
    mov rsi, rax

    mov rax, rbx
    mov rdx, rbx
    shr rdx, 32
    wrmsr                           ; active GS base = this core's PerCoreData

.par_gs_settled:
    push rbx                        ; slot: kernel GS base (0 = left as found)
    push rsi                        ; slot: GS base as found

    lea rdi, [rsp + 16]     ; arg0 = interrupt_frame_t* (skip the two slots)

    ; 16-byte align per SysV AMD64 ABI §3.2 before `call`.
    mov rax, rsp
    and rax, 15
    sub rsp, rax
    push rax                ; save the alignment correction

    ; Every paranoid vector is an exception (1, 2, 8, 12, 18 — all < 32),
    ; so dispatch directly. No IRQ / syscall path lives here.
    call exception_handler

    pop rax
    add rsp, rax            ; undo alignment correction

    pop rsi                 ; rsi = GS base as found at entry
    pop rbx                 ; rbx = this core's kernel GS base

    ; ---- Leave the GS state the FRAME is about to return to. ----
    ; Two facts, not one. Replaying the entry decision is only sound where the
    ; handler cannot change the ring it returns to, which is why Linux's
    ; paranoid_exit carries "this code does not execute at all for exceptions
    ; from user mode". Here exception_handler DOES reschedule — the #DF/#SS
    ; kernel-stack-overflow recovery in idt.c calls schedule(frame), and
    ; context_restore_to_frame writes frame->cs — so a kernel-mode entry can
    ; come out as a ring-3 frame.
    ;   ring 0 -> the interrupted code owns whatever base it had: write back
    ;     exactly what was found and leave the shadow alone. A swapgs window we
    ;     landed inside is handed back open, as we found it. If the entry took
    ;     no base (rbx = 0) there is nothing to put back either.
    ;   ring 3 -> the crossing must end with the user base active and the
    ;     kernel base in the shadow (notify.h). Out of ordinary kernel context
    ;     (found base == kernel base) the shadow holds the user base and
    ;     swapgs is the crossing — and that is also the case when the entry
    ;     took no base at all, because then GS was never disturbed and the
    ;     shadow still holds what the last crossing parked there. Out of an
    ;     interrupted swapgs window (found base != kernel base) the shadow
    ;     ALREADY holds the kernel base and the found base IS the user one, so
    ;     writing it back is the whole crossing — a swapgs there would hand
    ;     ring 3 this core's per-cpu data.
    ; The ring is therefore decided FIRST: a frame rewritten to ring 3 must
    ; cross whatever the entry did or did not do to GS.
    ; This runs before pop r15..rax so the register restore may clobber
    ; rsi/rbx; swapgs and WRMSR touch MSR state only, not the frame. After the
    ; two slot pops the CPU frame sits at: r15(+0) .. rax(+112) vector(+120)
    ; err(+128) rip(+136) cs(+144).
    test byte [rsp + 144], 3
    jz .par_exit_ring0
    test rbx, rbx
    jz .par_exit_swapgs     ; entry left GS as found; the shadow is untouched
    cmp rsi, rbx
    je .par_exit_swapgs     ; ordinary kernel context
    jmp .par_exit_restore   ; interrupted swapgs window: found base is the user one
.par_exit_ring0:
    test rbx, rbx
    jz .par_exit_gs_done    ; entry left GS as found; so does the exit
.par_exit_restore:
    mov ecx, MSR_IA32_GS_BASE
    mov rax, rsi
    mov rdx, rsi
    shr rdx, 32
    wrmsr
    jmp .par_exit_gs_done
.par_exit_swapgs:
    swapgs
.par_exit_gs_done:

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
    ; One per USB host controller (114-120). A vector with no stub of its own
    ; is the fault handler, which is how the very first xHCI interrupt this
    ; kernel ever received arrived as a #GP panic naming the wrong vector.
    dq isr114
    dq isr115
    dq isr116
    dq isr117
    dq isr118
    dq isr119
    dq isr120
    ; Unimplemented (121-127) - use GPF handler
    times 7 dq isr13
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
