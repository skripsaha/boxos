; AP trampoline - copied to physical 0x8000 before SIPI
; Transitions AP: real mode -> protected mode -> long mode -> kernel
; No ORG directive - all absolute references use 0x8000 + (label - ap_trampoline_start)

[BITS 16]

section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_data

; Offsets of data fields relative to ap_trampoline_start
; Used in both 32-bit and 64-bit mode to avoid absolute-address NASM warnings
; by doing arithmetic in registers instead.
%define DATA_OFFSET_CR3        (ap_trampoline_data - ap_trampoline_start)
%define DATA_OFFSET_STACK      (ap_trampoline_data - ap_trampoline_start + 8)
%define DATA_OFFSET_CORE_IDX   (ap_trampoline_data - ap_trampoline_start + 16)
%define DATA_OFFSET_EXTRA_CR4  (ap_trampoline_data - ap_trampoline_start + 20)
%define DATA_OFFSET_EXTRA_EFER (ap_trampoline_data - ap_trampoline_start + 24)
%define GDT_PTR_OFFSET         (ap_gdt_ptr - ap_trampoline_start)

; The Handrail — the minimal IDT this core holds while it climbs into the
; kernel, and the LastWord it leaves if it falls.
;
; Between the SIPI and per_core_init_ap's idt_load this core has no IDT of its
; own: after INIT the IDTR reads base 0, limit 0FFFFH (Intel SDM Vol 3A, Table
; 9-1 "Processor State Following Power-up, Reset, or INIT"). Linear 0 is not
; mapped in this kernel — vmm_init drops the loader's identity map and amp.c
; puts back exactly one page, this one — so an exception in that window fetches
; its gate descriptor from a page that is not present, takes #PF, fetches THAT
; gate from the same absent page, takes #DF, and resets the board. No line, no
; ring, no screen: the failure that is hardest to diagnose is the one this
; window produces.
;
; So the core carries a handrail. It leads nowhere — it services nothing, it
; returns from nothing — it only makes a fall say what it was. Every gate lands
; in a stub that writes the vector, the error code, the faulting RIP and CR2
; into the data area below, seals the record with the word `LastWord`, and
; stops the core. The BSP is already waiting for this core to report in; when
; it times out it reads the seal and speaks for the core that cannot.
;
; It does not lean on the core's own stack to do it. Every gate carries IST 1,
; which points at a ledge at the end of this same page, so the one fault a
; minimal table would otherwise be helpless against — the one whose stack is
; the thing that is broken — still lands somewhere it can write from. A page
; the core is executing out of is a page that is present, which is what makes
; the ledge the one piece of memory this record can always be written on.
;
; ‼ WHAT IS STILL NOT COVERED, said plainly so nobody trusts this further than
; it goes: the trampoline's own 16- and 32-bit run, above. A protected-mode
; gate is eight bytes where a 64-bit one is sixteen, so covering that stretch
; means a second, differently-shaped table. It is left uncovered deliberately:
; it is a straight line of about twenty instructions that reads nothing but the
; data area the BSP filled in, and both values in it that could fault —
; CR4.LA57 and EFER.NXE — are ones the BSP asked CPUID about before writing
; them there, for exactly this reason.
;
; The gates name selector 0x08, and the table below is ordered so that 0x08 is
; the 64-bit code segment. That is not cosmetic: this table stays live past
; per_core_load_gdt, which swaps the trampoline's GDT for the kernel's per-core
; one, and index 1 of the kernel's GDT is GDT_KERNEL_CODE — a 64-bit code
; segment too. A gate naming any other index would stop being deliverable at
; the instant the GDT changed.
%define HANDRAIL_GATE_COUNT    32
%define HANDRAIL_CODE_SELECTOR 0x08
%define HANDRAIL_GATE_PRESENT  0x8E   ; P=1, DPL=0, type=1110b (64-bit interrupt gate)
%define HANDRAIL_GATE_IST      1      ; the ledge, so a broken stack is survivable
%define HANDRAIL_PTR_OFFSET    (ap_handrail_ptr - ap_trampoline_start)
%define HANDRAIL_TSS_SELECTOR  0x20
%define HANDRAIL_TSS_ACCESS_OFFSET (ap_gdt - ap_trampoline_start + 0x20 + 5)

; The ledge. It holds one exception frame (five qwords the CPU pushes, plus the
; vector and error code the stub pushes: fifty-six bytes) and the handler that
; runs on it writes five values and halts, so it never nests and never grows.
; Sized to the next power of two above that, and no larger, because it is
; carved out of the one page the trampoline gets.
%define HANDRAIL_LEDGE_BYTES   256

%define DATA_OFFSET_LW_SEAL    (ap_trampoline_data - ap_trampoline_start + 32)
%define DATA_OFFSET_LW_VECTOR  (ap_trampoline_data - ap_trampoline_start + 40)
%define DATA_OFFSET_LW_ERRCODE (ap_trampoline_data - ap_trampoline_start + 48)
%define DATA_OFFSET_LW_RIP     (ap_trampoline_data - ap_trampoline_start + 56)
%define DATA_OFFSET_LW_CR2     (ap_trampoline_data - ap_trampoline_start + 64)

ap_trampoline_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Intel SDM Vol 2A "LGDT/LIDT": in 16-bit operand size the CPU
    ; truncates the descriptor base to 24 bits. ap_gdt_ptr stores a full
    ; 32-bit base (NASM `dd ...`), so without the o32 prefix the high
    ; byte would silently be cleared. Today base < 16 MB so it works,
    ; but on a hypothetical relocation > 16 MB it would silently fault;
    ; the prefix makes the contract match the data we encoded.
    o32 lgdt [0x8000 + GDT_PTR_OFFSET]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; 0x18 is the 32-bit code segment in the table below — see the note there
    jmp 0x18:(0x8000 + (ap_protected - ap_trampoline_start))

[BITS 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; CR4: PAE (bit 5) is mandatory before enabling long mode.
    ; Additionally:
    ;   PGE       (bit 7)  — global-page TLB entries. Kernel page table
    ;                        entries are flagged with the global bit, so
    ;                        enabling PGE here keeps them across CR3
    ;                        loads during the brief window before
    ;                        per_core_init_ap → enable_fpu reconfigures
    ;                        CR4 fully. Without it every CR3 load (e.g.
    ;                        the first scheduler context switch on this
    ;                        AP) flushes the entire TLB.
    ;   OSFXSR    (bit 9)  — x86_64 ABI mandates SSE2; an emitted
    ;                        SSE move in any C function called before
    ;                        enable_fpu would #UD without it.
    ;   OSXMMEXCPT(bit 10) — pairs with OSFXSR so SIMD FP exceptions
    ;                        deliver as #XF instead of #UD.
    mov eax, cr4
    or eax, (1 << 5)  | (1 << 7) | (1 << 9) | (1 << 10)
    ; extra_cr4 mask (DATA_OFFSET_EXTRA_CR4) — set by amp_boot_aps to:
    ;   bit 12 (LA57) when BSP enabled 5-level paging — must be set BEFORE
    ;          CR0.PG=1 (Intel SDM §4.5: CR4.LA57 cannot change while
    ;          paging is on).
    ; OR it in defensively in 32-bit mode here, then continue with paging
    ; enablement below. ebx must still hold 0x8000 for the data fetch.
    mov ebx, 0x8000
    or eax, [ebx + DATA_OFFSET_EXTRA_CR4]
    mov cr4, eax

    ; Load CR3 from data area using register arithmetic (avoids ABS warning)
    mov ebx, 0x8000
    mov eax, [ebx + DATA_OFFSET_CR3]
    mov cr3, eax

    ; EFER: LME is mandatory to reach long mode at all. NXE (bit 11) is NOT
    ; hardcoded here — it comes from the data area, because a CPU that does
    ; not enumerate CPUID.80000001h:EDX[20] raises #GP on a WRMSR that sets
    ; it, and "Execute Disable Bit" is a switch real firmware exposes. The
    ; BSP already asked (g_cpu_caps.has_nx) and wrote the answer in for us,
    ; the same way it writes the extra CR4 bits above. ebx must still hold
    ; 0x8000 for the fetch, and it does — nothing below touched it.
    mov ebx, 0x8000
    mov esi, [ebx + DATA_OFFSET_EXTRA_EFER]
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    or eax, esi
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    or eax, (1 << 16)
    mov cr0, eax

    ; 0x08 is the 64-bit code segment, and it is the one the Handrail's gates
    ; name as well, so that they survive per_core_load_gdt
    jmp 0x08:(0x8000 + (ap_long_mode - ap_trampoline_start))

[BITS 64]
ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Use rbx as base = 0x8000 to avoid absolute-address deprecation warnings
    mov rbx, 0x8000
    mov rsp, [rbx + DATA_OFFSET_STACK]

    ; Take the handrail, and put the ledge under it first.
    ;
    ; LTR marks its descriptor busy, and every core comes through this same
    ; trampoline in turn, so the second one would find a busy descriptor and
    ; take #GP on the instruction meant to make faults survivable. Writing the
    ; access byte back to "available 64-bit TSS" costs one store and removes
    ; the whole class.
    mov byte [rbx + HANDRAIL_TSS_ACCESS_OFFSET], 0x89
    mov ax, HANDRAIL_TSS_SELECTOR
    ltr ax

    ; The table goes up BEFORE the first call into C, which is where every
    ; instruction this window is dangerous for lives.
    lidt [rbx + HANDRAIL_PTR_OFFSET]

    ; ap_entry_c(core_index, stack_top)
    ;   RDI = core_index  (first arg)
    ;   RSI = stack_top    (second arg)
    mov rsi, rsp                            ; RSI = stack top (before call pushes retaddr)
    xor rdi, rdi
    mov dil, [rbx + DATA_OFFSET_CORE_IDX]

    extern ap_entry_c
    mov rax, ap_entry_c
    call rax

.halt:
    cli
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; Handrail stubs. One per architectural vector, so the record can name WHICH
; fault happened rather than only that one did. Ten of the thirty-two arrive
; with an error code the CPU pushed (Intel SDM Vol 3A, Table 6-1 "Protected-
; Mode Exceptions and Interrupts", column "Error Code"); the rest get a zero
; pushed here so every stub hands the common tail the same shape.
; ---------------------------------------------------------------------------
%assign vec 0
%rep HANDRAIL_GATE_COUNT
ap_handrail_isr_ %+ vec:
%if vec==8 || vec==10 || vec==11 || vec==12 || vec==13 || vec==14 || vec==17 || vec==21 || vec==29 || vec==30
    ; the CPU pushed an error code for this vector — leave it where it is
%else
    push qword 0
%endif
    push qword vec
    jmp ap_handrail_common
%assign vec vec+1
%endrep

; Stack here, from the top: vector, error code, and then the frame the CPU
; itself pushed — RIP, CS, RFLAGS, RSP, SS.
ap_handrail_common:
    mov rbx, 0x8000

    mov rax, [rsp + 0]
    mov [rbx + DATA_OFFSET_LW_VECTOR], rax
    mov rax, [rsp + 8]
    mov [rbx + DATA_OFFSET_LW_ERRCODE], rax
    mov rax, [rsp + 16]
    mov [rbx + DATA_OFFSET_LW_RIP], rax
    mov rax, cr2
    mov [rbx + DATA_OFFSET_LW_CR2], rax

    ; The seal goes down LAST, and it is a word rather than a number so that
    ; nobody has to look up what it means: the BSP reads these eight bytes as
    ; the text `LastWord`, and four numbers written without it are four numbers
    ; from some earlier boot that nobody should believe.
    mov rax, 'LastWord'
    mov [rbx + DATA_OFFSET_LW_SEAL], rax

.stop:
    cli
    hlt
    jmp .stop

align 16
ap_handrail_idt:
%assign vec 0
%rep HANDRAIL_GATE_COUNT
    dw  (0x8000 + (ap_handrail_isr_ %+ vec - ap_trampoline_start)) & 0xFFFF
    dw  HANDRAIL_CODE_SELECTOR
    db  HANDRAIL_GATE_IST
    db  HANDRAIL_GATE_PRESENT
    dw  ((0x8000 + (ap_handrail_isr_ %+ vec - ap_trampoline_start)) >> 16) & 0xFFFF
    dd  ((0x8000 + (ap_handrail_isr_ %+ vec - ap_trampoline_start)) >> 32)
    dd  0
%assign vec vec+1
%endrep
ap_handrail_idt_end:

ap_handrail_ptr:
    dw (ap_handrail_idt_end - ap_handrail_idt - 1)
    dq 0x8000 + (ap_handrail_idt - ap_trampoline_start)

; The Handrail's TSS. Almost all of it is zero on purpose: this core takes no
; ring transition while the table above is live, so RSP0/1/2 are never
; consulted, and only IST 1 is ever named by a gate. The layout is Intel SDM
; Vol 3A, Figure "64-Bit TSS Format" — RSP0 at offset 4, the seven IST entries
; from offset 36, and the I/O map base last.
align 16
ap_handrail_tss:
    dd 0                                                          ; reserved
    dq 0                                                          ; RSP0
    dq 0                                                          ; RSP1
    dq 0                                                          ; RSP2
    dq 0                                                          ; reserved
    dq 0x8000 + (ap_handrail_ledge_top - ap_trampoline_start)     ; IST1 — the ledge
    dq 0                                                          ; IST2
    dq 0                                                          ; IST3
    dq 0                                                          ; IST4
    dq 0                                                          ; IST5
    dq 0                                                          ; IST6
    dq 0                                                          ; IST7
    dq 0                                                          ; reserved
    dw 0                                                          ; reserved
    ; I/O map base pointed past the end of the TSS: this core issues no port
    ; access under this table, and a base beyond the limit is how the SDM says
    ; to mean "there is no map".
    dw (ap_handrail_tss_end - ap_handrail_tss)
ap_handrail_tss_end:

align 16
ap_handrail_ledge:
    times HANDRAIL_LEDGE_BYTES db 0
ap_handrail_ledge_top:

align 16
; ‼ The order of these three is load-bearing, and it is not the obvious one.
;
; 0x08 is the 64-bit code segment and 0x18 is the 32-bit one, which is the
; other way round from how this table used to read. The reason is the Handrail:
; its gates have to name a selector, and that selector keeps being used after
; per_core_load_gdt has swapped this table out for the kernel's per-core GDT —
; the kernel's IDT does not arrive until per_core_load_tss has run, two calls
; later. In the kernel's GDT, index 1 (0x08) is GDT_KERNEL_CODE, a 64-bit code
; segment, and index 3 (0x18) is the USER DATA segment. Gates naming 0x18
; would have gone from deliverable to #GP the instant the GDT changed, and a
; #GP delivered through a gate that itself #GPs is a triple fault — the exact
; failure the Handrail exists to prevent, moved two lines later.
;
; Naming 0x08 in both tables means the same thing in both, so the Handrail
; stays live across the switch, all the way to idt_load.
ap_gdt:
    dq 0
    ; 0x08: 64-bit code — the far jump into long mode, and the selector every
    ;       Handrail gate names. Index 1 in the kernel's GDT is the same kind
    ;       of segment, which is what lets the Handrail outlive this table.
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
    ; 0x10: data (32 and 64-bit) — index 2 is GDT_KERNEL_DATA in the kernel's
    ;       GDT as well, so DS/ES/SS stay meaningful across the switch too.
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
    ; 0x18: 32-bit code — used by exactly one instruction, the far jump out of
    ;       real mode, and never again.
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 0x20: the Handrail's TSS. A 64-bit system descriptor is sixteen bytes
    ;       (Intel SDM Vol 3A, "TSS Descriptor in 64-bit mode"), and the access
    ;       byte 0x89 is P=1, DPL=0, type=1001b — an available 64-bit TSS. The
    ;       stub below rewrites this byte before every LTR, because LTR marks
    ;       the descriptor busy and the next core to come through this same
    ;       trampoline would take a #GP on an already-busy one.
    dw (ap_handrail_tss_end - ap_handrail_tss - 1)
    dw (0x8000 + (ap_handrail_tss - ap_trampoline_start)) & 0xFFFF
    db ((0x8000 + (ap_handrail_tss - ap_trampoline_start)) >> 16) & 0xFF
    db 0x89
    db 0x00
    db ((0x8000 + (ap_handrail_tss - ap_trampoline_start)) >> 24) & 0xFF
    dd ((0x8000 + (ap_handrail_tss - ap_trampoline_start)) >> 32)
    dd 0

ap_gdt_ptr:
    dw (ap_gdt_ptr - ap_gdt - 1)
    dd 0x8000 + (ap_gdt - ap_trampoline_start)

align 8
ap_trampoline_data:
    dq 0    ; +0:  CR3 (physical PML4 / PML5 address)
    dq 0    ; +8:  AP stack top (virtual)
    db 0    ; +16: core_index
    db 0    ; +17: padding
    db 0    ; +18: padding
    db 0    ; +19: padding
    dd 0    ; +20: extra CR4 bits (LA57 when 5-level paging is active on BSP)
    dd 0    ; +24: extra EFER bits (NXE when this CPU enumerates NX)
    dd 0    ; +28: padding, so the LastWord below starts eight-byte aligned

    ; The LastWord. Written by a Handrail stub, read by the BSP when the core
    ; it belongs to never reports in. The BSP clears the seal before each SIPI,
    ; so a sealed record always belongs to the core it is waiting on and never
    ; to the one before it.
    dq 0    ; +32: seal — the eight bytes `LastWord` once the record is whole
    dq 0    ; +40: vector
    dq 0    ; +48: error code (zero where the CPU pushes none)
    dq 0    ; +56: RIP the fault was taken at
    dq 0    ; +64: CR2, meaningful for #PF and stale otherwise

ap_trampoline_end:
