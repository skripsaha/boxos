[BITS 64]

; void TagBootJump(uint64_t pt_base, uint64_t stack, uint64_t entry)
;
; System V AMD64 ABI:
;   rdi = pt_base   — physical address of our PML4
;   rsi = stack     — boot stack base (grows downward)
;   rdx = entry     — kernel entry point (0x100000)
;
; UEFI is already in 64-bit long mode (EFER.LME=1, CR0.PG=1, CR4.PAE=1).
; All we need is to install our CR3, switch to the boot stack, and jump.
; This function never returns.
;
; IMPORTANT: do NOT use rdmsr/wrmsr here — rdmsr clobbers rdx (entry point).

global TagBootJump

TagBootJump:
    ; CR4: ensure PAE (bit 5) — already set by UEFI, enforce to be safe
    mov rax, cr4
    or  rax, 0x20
    mov cr4, rax

    ; CR3: install our page tables (TLB flush happens automatically).
    ; After this instruction, the CPU uses our identity + higher-half tables.
    ; rdx (entry point) is preserved — we do NOT call rdmsr.
    mov cr3, rdi

    ; Switch to boot stack and clear frame pointer
    mov rsp, rsi
    xor rbp, rbp

    ; Jump to kernel entry point (rdx = 0x100000, unmodified)
    jmp rdx
