[BITS 64]


global TagBootJump

TagBootJump:
    mov rax, cr4
    or  rax, 0x20
    mov cr4, rax

    mov cr3, rdi

    mov rsp, rsi
    xor rbp, rbp

    jmp rdx