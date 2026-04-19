[BITS 64]

; void TagBootJump(uint64_t pt_base, uint64_t stack, uint64_t entry)
;
; System V AMD64 ABI:
;   rdi = pt_base   — physical address of our PML4
;   rsi = stack     — boot stack base (grows downward)
;   rdx = entry     — kernel entry point (0x100000)
;
; UEFI is already in long mode. We swap the page tables and jump.
; This function never returns.

global TagBootJump

TagBootJump:
    ; CR4: ensure PAE (bit 5) — already set by UEFI, enforce to be safe
    mov rax, cr4
    or  rax, 0x20
    mov cr4, rax

    ; CR3: install our page tables (TLB flush happens automatically)
    mov cr3, rdi

    ; EFER MSR (0xC0000080): ensure LME (bit 8) + NXE (bit 11)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 0x900
    wrmsr

    ; CR0: ensure PG (bit 31)
    mov rax, cr0
    or  eax, 0x80000000
    mov cr0, rax

    ; Switch to boot stack; clear frame pointer
    mov rsp, rsi
    xor rbp, rbp

    ; Jump to kernel entry point
    jmp rdx
