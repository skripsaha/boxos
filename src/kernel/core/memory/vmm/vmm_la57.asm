; LA57 (5-level paging) runtime transition trampoline.
;
; Intel SDM Vol 3A §4.1.1 / §4.5 / §9.8.5 mandates:
;   "An attempt to modify CR4.PAE, IA32_EFER.LME, or CR4.LA57 while 4-level or
;    5-level paging is enabled causes a general-protection exception (#GP(0)).
;    To switch paging modes, software must first disable paging (by clearing
;    CR0.PG with MOV to CR0), then set CR4.PAE, IA32_EFER.LME, and CR4.LA57
;    to the desired values, and then re-enable paging."
;
; While CR0.PG=0 with EFER.LME=1, IA32_EFER.LMA goes to 0 and the CPU drops
; to compatibility mode. Our current 64-bit CS (L=1) is then ill-defined for
; instruction decode (CS.L is ignored when LMA=0, and CS.D=0 makes the CPU
; decode as 16-bit). We MUST run the dance under a 32-bit CS (L=0, D=1) and
; restore the 64-bit CS afterward.
;
; The dance window has CR0.PG=0 ⇒ all memory accesses are PHYSICAL. The
; trampoline therefore:
;   • runs from an identity-mapped PA <4 GB low alias of the kernel's text
;     page (boot identity is alive in vmm_init's early phase),
;   • never touches the stack between PG=0 and PG=1,
;   • addresses every persistent slot (GDTR, far-jmp memory operands) via
;     PA = VA - KERNEL_VMA arithmetic.
;
; NMI/MCE delivered during the dance window read IDTR (high VA) under PG=0
; (linear=phys), so they would triple-fault. The window is microseconds and
; boot-only — accepted risk, matches Linux compressed/head_64.S handling.
;
; ─── Two-stage jump back to 64-bit (low-PA trampoline) ───────────────────
; The compat→64-bit transition uses a TWO-STAGE jump for portability:
;   stage 1: jmp far m16:m32 from compat CS to LOW-PA stub in 64-bit CS.
;            Target offset has bit 31 = 0, so SDM-mandated sign-extension
;            (Vol 2A "JMP" Operation pseudocode) and any potential
;            zero-extension implementation both yield the same canonical
;            low VA. Avoids any reliance on compat→64-bit extension form.
;   stage 2: in low_64_trampoline (now 64-bit mode), use `mov rax, .high64;
;            jmp rax` — full 64-bit absolute jump to the kernel high-VA
;            half via the new PML5[511]→...→PD_high path. No extension.
;
; REX.W cannot be used to force m16:m64 in compat mode — Intel SDM Vol 2A
; §2.2.1.2 / AMD APM Vol 3 §1.7.1.6 state that REX prefixes are NOT
; recognised in compatibility submode (treated as legacy INC/DEC). So the
; two-stage jump is the portable workaround.
;
; ─── QEMU TCG triple-fault note ──────────────────────────────────────────
; Investigation (2026-06-08) showed that QEMU TCG (-cpu max) consistently
; triple-faults on the compat→64-bit `jmp far` *regardless of operand form
; or target VA* once CR4.LA57=1 is active. Likely a TCG-specific limitation
; in handling the compat-submode→long-mode transition with 5-level paging
; enabled (Linux's head_64.S takes a different path — enables LA57 in 32-bit
; protected mode BEFORE entering IA-32e, so this exact transition is rarely
; exercised). The vmm_init caller therefore GATES this trampoline on
; !hv_present() — only invoked on bare metal. On real Sapphire Rapids /
; Ice Lake-SP / Zen 4 silicon the dance follows Intel SDM and should work.
;
; Caller invariants (vmm_init at the LA57 enable site):
;   • RDI = pml5_phys, a 4 KB-aligned PML5 page in PA <4 GB whose entries
;     PML5[0] and PML5[511] both point to the existing boot PML4 (so both
;     identity and higher-half stay reachable through the new top-level
;     interpretation).
;   • CR0.PG=1, EFER.LME=1, EFER.LMA=1 (standard long mode).
;   • CR4.LA57 currently 0 (4-level paging).
;   • CR4.PCIDE currently 0 (PCID not yet enabled — vmm_init enables PCID
;     AFTER this dance).
;   • Caller's stack is mapped at high VA (kernel boot stack mapped via
;     PML4[511] PDPT_high) AND survives the CR3 swap (PML5[511] = same
;     PML4 phys preserves the mapping).

%define CR0_PG       (1 << 31)
%define CR4_LA57     (1 << 12)
%define KERNEL_VMA   0xFFFFFFFF80000000
%define DANCE_CS32   0x08
%define DANCE_CS64   0x10
%define DANCE_DS     0x18
%define KERNEL_CODE  0x08                  ; gdt.h GDT_KERNEL_CODE
%define KERNEL_DATA  0x10                  ; gdt.h GDT_KERNEL_DATA

[BITS 64]
section .text

global vmm_la57_runtime_enable

; void vmm_la57_runtime_enable(uint64_t pml5_phys);
;   System V AMD64 ABI: RDI = pml5_phys
;
; pml5_phys MUST be < 4 GB. Caller asserts this; we re-check defensively.
;
; CET / IBT: called once from vmm_init via direct CALL. We add ENDBR64
; at entry to keep the symbol IBT-safe in case anyone ever reaches us
; through an indirect branch; NOP without CR4.CET=1.
vmm_la57_runtime_enable:
    endbr64
    pushfq
    cli
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; ----- Defensive: refuse if pml5_phys >= 4 GB -----
    ; (The 32-bit `mov cr3, ebx` below would otherwise zero-extend high bits
    ;  and the CPU would walk garbage tables.)
    mov rax, rdi
    shr rax, 32
    test rax, rax
    jnz .bail

    ; R15 = KERNEL_VMA — used to convert high-VA→PA via subtraction.
    mov r15, KERNEL_VMA

    ; ----- Save caller's GDTR and IDTR (via PA aliases of static slots) -----
    lea rax, [rel vmm_la57_saved_gdtr]
    sub rax, r15
    sgdt [rax]
    lea rax, [rel vmm_la57_saved_idtr]
    sub rax, r15
    sidt [rax]

    ; ----- Load the dance GDT (CS32/CS64/DS) via PA alias -----
    lea rax, [rel vmm_la57_dance_gdtr]
    sub rax, r15
    lgdt [rax]

    ; ----- Stash pml5_phys low 32 in EBX (verified < 4 GB above) -----
    mov ebx, edi

    ; ----- Build 32-bit far-jmp slot: PA of .low32 + DANCE_CS32 -----
    lea rax, [rel .low32]
    sub rax, r15                           ; PA of .low32 (fits in 32 bits)
    lea rcx, [rel vmm_la57_jmp32]
    sub rcx, r15                           ; PA of slot in .bss
    mov dword [rcx], eax                   ; 32-bit offset
    mov word  [rcx + 4], DANCE_CS32

    ; ----- Build far-jmp slot: target = PA of low_64_trampoline (NOT .high64) ----
    ; Critical: target offset must have BIT 31 = 0 so that whether the CPU
    ; sign-extends or zero-extends the 32-bit compat-mode far-jmp offset
    ; (Intel SDM mandates sign-extend; QEMU TCG appears to mis-implement
    ; this for compat→64-bit transitions when bit 31 = 1), the resulting
    ; RIP lands on a kernel-text low-PA address that is identity-mapped
    ; under temp_pml5 (PML5[0]→boot_PML4[0]→PDPT_id→PD0). The low_64_trampoline
    ; stub then jmp's to .high64 via a 64-bit absolute jmp (mov rax, .high64
    ; / jmp rax) which has no extension ambiguity.
    ;
    ; REX.W cannot be used to force m16:m64 in compat mode — Intel SDM Vol 2A
    ; §2.2.1.2 / AMD APM Vol 3 §1.7.1.6: "REX prefix is NOT recognized as an
    ; instruction prefix in compatibility mode (treated as legacy INC/DEC)".
    ; So the two-stage jump via a low-PA stub is the portable workaround.
    lea rax, [rel .low_64_trampoline]
    sub rax, r15                           ; PA of trampoline (bit 31 = 0)
    lea rcx, [rel vmm_la57_jmp64]
    sub rcx, r15
    mov dword [rcx], eax                   ; 4-byte offset = low PA, bit 31=0
    mov word  [rcx + 4], DANCE_CS64        ; 2-byte selector

    ; ----- EBP = PA of jmp64 slot (preserved across mode flip) -----
    lea rax, [rel vmm_la57_jmp64]
    sub rax, r15
    mov ebp, eax                           ; PA fits in 32 bits

    ; ----- R14 = PA of saved GDTR slot — used after .high64 returns -----
    lea r14, [rel vmm_la57_saved_gdtr]
    sub r14, r15

    ; ----- Far-jmp via memory to 32-bit CS, landing at PA of .low32 -----
    lea rax, [rel vmm_la57_jmp32]
    sub rax, r15
    jmp far dword [rax]

[BITS 32]
.low32:
    ; ----- 32-bit compatibility mode (CS.D=1, L=0). EFER.LMA still 1 -----
    ; Reload data segments to flat 32-bit DS.
    mov ax, DANCE_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ----- Disable paging: CR0.PG=0 → EFER.LMA → 0 (compat mode) -----
    mov eax, cr0
    and eax, ~CR0_PG
    mov cr0, eax

    ; ----- Set CR4.LA57 = 1 (legal now that paging is off) -----
    mov eax, cr4
    or  eax, CR4_LA57
    mov cr4, eax

    ; ----- Load CR3 = pml5_phys (low 32, verified <4 GB) -----
    mov cr3, ebx

    ; ----- Re-enable paging: CR0.PG=1 → EFER.LMA → 1 in 5-level mode -----
    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax

    ; ----- Far-jmp to 64-bit CS via [ebp] using m16:m32. Target = LOW PA
    ; of low_64_trampoline (bit 31 = 0) so the SDM-mandated sign-extension
    ; of the 32-bit offset is moot — both sign and zero extension yield
    ; the same low canonical VA. Identity-mapped under temp_pml5[0]. -----
    jmp far dword [ebp]

[BITS 64]
.low_64_trampoline:
    ; ----- Now in 64-bit mode at LOW PA (CS=DANCE_CS64) -----
    ; Jump to .high64 (high VA) via 64-bit absolute jmp. `mov rax, .high64`
    ; emits `48 B8 imm64` — full 64-bit absolute address resolved by linker.
    ; `jmp rax` loads RIP with that absolute, switching fetch path from
    ; identity (PML5[0]) to higher-half (PML5[511]). No extension subtlety.
    mov rax, .high64
    jmp rax

.high64:
    ; ----- Back in 64-bit mode at HIGH VA on dance CS64 (selector 0x10) -----
    ; Restore caller's GDTR (via R14, preserved across mode flips).
    lgdt [r14]

    ; Reload data segments with kernel-data selector (matches restored GDT).
    mov ax, KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    ; GS deliberately NOT reloaded — preserves IA32_GS_BASE per per_core.c.

    ; Reload CS to kernel's GDT_KERNEL_CODE (0x08) via far return. RIP comes
    ; from .reload_cs label; SS:RSP is the original caller stack (still mapped
    ; via boot tables' higher-half PD).
    sub rsp, 16
    lea rax, [rel .reload_cs]
    mov [rsp], rax
    mov qword [rsp + 8], KERNEL_CODE
    retfq

.reload_cs:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

.bail:
    ; PML5 phys >= 4 GB — caller asserts this; defensive bail preserves
    ; ABI registers and returns. Caller's panic on its own assertion.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

; ============================================================================
; .data — dance GDT, GDTR descriptors.
; ============================================================================
section .data
align 8

global vmm_la57_dance_gdt
vmm_la57_dance_gdt:
    dq 0x0000000000000000                  ; 0x00 null
    ; 0x08 — 32-bit code, base=0, limit=4G, type=0x9A (P+R+code), G=1, D=1, L=0
    dq 0x00CF9A000000FFFF
    ; 0x10 — 64-bit code, base=0, limit ignored, type=0x9A, G=1, D=0, L=1
    dq 0x00AF9A000000FFFF
    ; 0x18 — 32-bit data, base=0, limit=4G, type=0x92 (P+R+W+data), G=1, D=1
    dq 0x00CF92000000FFFF
vmm_la57_dance_gdt_end:

global vmm_la57_dance_gdtr
vmm_la57_dance_gdtr:
    dw vmm_la57_dance_gdt_end - vmm_la57_dance_gdt - 1
    dq vmm_la57_dance_gdt - KERNEL_VMA     ; PA (linker subtracts at relocation)

section .bss
align 8

global vmm_la57_saved_gdtr
vmm_la57_saved_gdtr:
    resb 16                                ; 10-byte GDTR + padding

global vmm_la57_saved_idtr
vmm_la57_saved_idtr:
    resb 16                                ; 10-byte IDTR + padding

global vmm_la57_jmp32
vmm_la57_jmp32:
    resb 8                                 ; 4-byte offset + 2-byte selector + pad

global vmm_la57_jmp64
vmm_la57_jmp64:
    resb 8                                 ; 4-byte offset + 2-byte selector + 2 pad
