[BITS 16]
[ORG 0x7C00]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000      ; below stage1 (0x7C00), above E820 area (~0x1104): safe 24KB stack

    mov [boot_drive], dl

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc no_lba

    mov si, dap_stage2
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov ax, [0x8000]
    cmp ax, 0x2907
    jne stage2_error

    jmp 0x0000:0x8000

no_lba:
    mov si, msg_no_lba
    call print
    jmp hang

disk_error:
    mov si, msg_disk_error
    call print
    jmp hang

stage2_error:
    mov si, msg_stage2_error
    call print
    jmp hang

hang:
    cli
    hlt
    jmp hang

print:
    push ax
    push bx
    mov ah, 0x0E
    mov bh, 0
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

boot_drive db 0

align 4
dap_stage2:
    db 0x10             ; Size
    db 0                ; Reserved
    dw 16               ; Sectors (Stage2)
    dw 0x8000           ; Offset
    dw 0x0000           ; Segment
    dq 1                ; LBA sector 1

msg_no_lba      db "LBA required!", 13, 10, 0
msg_disk_error  db "Disk error!", 13, 10, 0
msg_stage2_error db "Stage2 fail!", 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55
