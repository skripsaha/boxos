[BITS 64]
[EXTERN kernel_main]

section .text
global _start

; Port I/O functions
global write_port
global read_port

; GDT functions
global get_gdt_base
global load_gdt

; VGA functions
global clear_screen_vga
global hide_cursor

_start:
    ; Debug output: Kernel entry started
    mov al, 'K'
    mov dx, 0x3f8
    out dx, al

    ; Clear segment registers
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Setup stack AFTER BSS section (BSS ends at ~0x810000, stack at 0x900000 = 9MB)
    ; CRITICAL: Stack must be AFTER BSS to avoid corruption!
    mov rsp, 0x900000
    mov rbp, rsp

    ; Debug output: Stack setup complete
    mov al, 'S'
    mov dx, 0x3f8
    out dx, al

    mov al, 'M'
    mov dx, 0x3f8
    out dx, al

    ; === BSS INITIALIZATION (BUG #1 FIX) ===
    ; Zero out BSS section to initialize global variables
    ; Critical: C standard requires uninitialized globals be zero
    extern __bss_start
    extern __bss_end

    mov rdi, __bss_start          ; Start of BSS section
    mov rcx, __bss_end            ; End of BSS section
    sub rcx, rdi                  ; Calculate size in bytes
    xor rax, rax                  ; Zero value to write
    rep stosb                     ; Clear BSS (RDI += RCX bytes, RCX=0)

    ; Debug output: BSS cleared
    mov al, 'Z'
    mov dx, 0x3f8
    out dx, al

    ; Parameters already set by Stage2 bootloader:
    ; RDI = E820 map address (0x500)
    ; RSI = E820 entry count
    ; RDX = Available memory start (set to 1MB)
    mov rdx, 0x100000

    ; Debug output: About to call kernel_main
    mov al, 'C'
    mov dx, 0x3f8
    out dx, al

    ; Debug output: Calling kernel_main NOW
    mov al, 'J'
    mov dx, 0x3f8
    out dx, al

    ; Call kernel main function
    call kernel_main

    ; Debug output: kernel_main returned (should never happen)
    mov al, 'R'
    mov dx, 0x3f8
    out dx, al

    ; Halt if kernel returns
    cli
    hlt
    jmp $

; ===========================================================================
; Port I/O Functions
; ===========================================================================

; Write byte to port
; Arguments: RDI = port, RSI = byte
write_port:
    mov dx, di
    mov al, sil
    out dx, al
    ret

; Read byte from port
; Arguments: RDI = port
; Returns: RAX = byte read
read_port:
    mov dx, di
    in al, dx
    movzx eax, al
    ret

; ===========================================================================
; GDT Functions
; ===========================================================================

; Get GDT base address
; Returns: RAX = GDT base address
get_gdt_base:
    sub rsp, 16
    sgdt [rsp]
    mov rax, [rsp + 2]  ; Skip limit (2 bytes), get base
    add rsp, 16
    ret

; Load new GDT
; Arguments: RDI = pointer to GDT descriptor
load_gdt:
    lgdt [rdi]

    ; Reload segment registers
    mov ax, 0x20        ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS through far return
    push 0x18           ; Kernel code segment
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

; ===========================================================================
; VGA Functions
; ===========================================================================

; Print string to VGA text buffer
; Arguments: RDI = string pointer, AH = color, RCX = screen offset
print_string_vga:
    push rdi
    push rbx
    push rax
    push rcx

    mov rbx, 0xB8000
    add rbx, rcx

.print_loop:
    mov al, [rdi]
    test al, al
    jz .done

    mov [rbx], ax
    add rbx, 2
    inc rdi
    jmp .print_loop

.done:
    pop rcx
    pop rax
    pop rbx
    pop rdi
    ret

; Clear VGA screen
clear_screen_vga:
    push rdi
    push rcx
    push rax

    mov rdi, 0xB8000
    mov rcx, 2000
    mov al, ' '
    mov ah, 0x07
    rep stosw

    pop rax
    pop rcx
    pop rdi
    ret

; Hide hardware cursor
hide_cursor:
    mov dx, 0x3D4
    mov al, 0x0E
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al

    mov dx, 0x3D4
    mov al, 0x0F
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al
    ret
