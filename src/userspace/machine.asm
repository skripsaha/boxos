; What this processor says of itself, asked with cpuid, and the string, bit
; and atomic instructions at work on what it says.

section .bss
vendor  resb 16
brand   resb 64
copy    resb 64
counter resq 1

section .rodata
said_vendor db "vendor: %s", 10, 0
said_brand  db "brand:  %s", 10, 0
no_brand    db "brand:  (this processor does not say)", 10, 0
said_length db "the brand is %d characters long", 10, 0
said_bits   db "bsr(1000) = %d, bswap(0x11223344) = 0x%x", 10, 0
said_popcnt db "popcnt(0xF0F0) = %d", 10, 0
no_popcnt   db "popcnt: this processor does not have it", 10, 0
said_count  db "lock xadd counted to %d", 10, 0

section .text
extern printf
global main

main:
    push rbx                    ; cpuid writes ebx, which main's caller keeps
    cld                         ; string instructions walk upward

    xor eax, eax                ; leaf 0: the vendor, in ebx, edx, ecx
    cpuid
    mov [vendor], ebx
    mov [vendor + 4], edx
    mov [vendor + 8], ecx
    lea rdi, [said_vendor]
    lea rsi, [vendor]
    xor eax, eax
    call printf

    mov eax, 0x80000000         ; the highest extended leaf
    cpuid
    cmp eax, 0x80000004
    jb .no_brand
    lea rdi, [brand]
    mov esi, 0x80000002         ; three leaves, sixteen bytes each
.leaf:
    mov eax, esi
    cpuid
    mov [rdi], eax
    mov [rdi + 4], ebx
    mov [rdi + 8], ecx
    mov [rdi + 12], edx
    add rdi, 16
    inc esi
    cmp esi, 0x80000005
    jne .leaf

    lea rsi, [brand]            ; copied whole, then the copy measured
    lea rdi, [copy]
    mov ecx, 48
    rep movsb
    lea rdi, [copy]
    xor eax, eax
    mov rcx, -1
    repne scasb                 ; rcx counts down past the NUL
    not rcx
    dec rcx
    mov rbx, rcx

    lea rdi, [said_brand]
    lea rsi, [copy]
    xor eax, eax
    call printf
    lea rdi, [said_length]
    mov esi, ebx
    xor eax, eax
    call printf
    jmp .bits
.no_brand:
    lea rdi, [no_brand]
    xor eax, eax
    call printf

.bits:
    mov eax, 1000
    bsr esi, eax
    mov edx, 0x11223344
    bswap edx
    lea rdi, [said_bits]
    xor eax, eax
    call printf

    mov eax, 1                  ; leaf 1: ecx bit 23 says popcnt is there
    cpuid
    bt ecx, 23
    jnc .no_popcnt
    mov eax, 0xF0F0
    popcnt esi, eax
    lea rdi, [said_popcnt]
    xor eax, eax
    call printf
    jmp .count
.no_popcnt:
    lea rdi, [no_popcnt]
    xor eax, eax
    call printf

.count:
    mov ecx, 10
.add:
    mov eax, 1
    lock xadd [counter], rax
    loop .add
    lea rdi, [said_count]
    mov rsi, [counter]
    xor eax, eax
    call printf

    pop rbx
    box.exit 0
