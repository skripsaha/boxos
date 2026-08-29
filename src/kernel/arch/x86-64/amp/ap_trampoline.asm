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

    jmp 0x08:(0x8000 + (ap_protected - ap_trampoline_start))

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

    jmp 0x18:(0x8000 + (ap_long_mode - ap_trampoline_start))

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

align 16
ap_gdt:
    dq 0
    ; 0x08: 32-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 0x10: data (32 and 64-bit)
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
    ; 0x18: 64-bit code
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00

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

ap_trampoline_end:
