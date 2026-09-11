[BITS 16]
[ORG 0x7C00]

SCRATCH         equ 0x1200
S_DRIVE         equ SCRATCH + 0x00
S_EDD           equ SCRATCH + 0x01
S_SPT           equ SCRATCH + 0x02
S_HEADS         equ SCRATCH + 0x04
S_DAP           equ SCRATCH + 0x08
SCRATCH_WORDS   equ 12

STAGE2_SEG      equ 0x0000
STAGE2_OFF      equ 0x8000

STAGE2_LBA_OFFSET equ 432
%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must come from the build (-DSTAGE2_SECTORS=...)"
%endif
STAGE2_SIG      equ 0x2907
RETRIES         equ 5

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov di, SCRATCH
    mov cx, SCRATCH_WORDS
    rep stosw

    mov [S_DRIVE], dl

    mov al, 'B'
    call putc

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [S_DRIVE]
    int 0x13
    jc .no_edd
    cmp bx, 0xAA55
    jne .no_edd
    test cl, 1
    jz .no_edd
    mov byte [S_EDD], 1
.no_edd:

    push es
    xor ax, ax
    mov es, ax
    xor di, di
    mov ah, 0x08
    mov dl, [S_DRIVE]
    int 0x13
    pop es
    jc .no_geom
    movzx ax, cl
    and ax, 0x3F
    mov [S_SPT], ax
    movzx ax, dh
    inc ax
    mov [S_HEADS], ax
.no_geom:

    mov eax, [stage2_lba]
    mov cx, STAGE2_SECTORS
    mov bx, STAGE2_OFF
    mov dx, STAGE2_SEG
    call read_sectors
    mov al, 'D'
    jc die_status

    mov ax, [STAGE2_OFF + 2]
    cmp ax, STAGE2_SIG
    mov al, 'S'
    jne die_char

    mov dl, [S_DRIVE]
    jmp STAGE2_SEG:STAGE2_OFF

read_sectors:
    pushad
    push es

    mov [S_DAP + 8], eax
    mov [rs_left], cx
    mov [S_DAP + 4], bx
    mov [S_DAP + 6], dx
    mov byte [S_DAP + 0], 0x10
    mov byte [rs_try], RETRIES

.next:
    cmp word [rs_left], 0
    je .done

    cmp byte [S_EDD], 1
    je .via_edd

    cmp word [S_SPT], 0
    je .exhausted

    mov eax, [S_DAP + 8]
    xor edx, edx
    movzx ecx, word [S_SPT]
    div ecx
    inc dl
    mov bl, dl
    xor edx, edx
    movzx ecx, word [S_HEADS]
    div ecx
    mov dh, dl
    cmp eax, 1023
    ja .exhausted

    mov ch, al
    mov cl, ah
    shl cl, 6
    or  cl, bl
    mov dl, [S_DRIVE]
    mov ax, [S_DAP + 6]
    mov es, ax
    mov bx, [S_DAP + 4]
    mov ax, 0x0201
    int 0x13
    jc .retry
    cmp al, 1
    jne .short
    jmp .advance

.via_edd:
    mov word [S_DAP + 2], 1
    mov si, S_DAP
    mov ah, 0x42
    mov dl, [S_DRIVE]
    int 0x13
    jc .retry
    cmp word [S_DAP + 2], 1
    jne .short

.advance:
    inc dword [S_DAP + 8]
    add word [S_DAP + 6], 0x20
    dec word [rs_left]
    mov byte [rs_try], RETRIES
    jmp .next

.short:
    xor ah, ah
.retry:
    mov [rs_err], ah
    dec byte [rs_try]
    jz .exhausted
    xor ax, ax
    mov dl, [S_DRIVE]
    int 0x13
    jmp .next

.done:
    pop es
    popad
    clc
    ret

.exhausted:
    cmp byte [S_EDD], 1
    jne .give_up
    mov byte [S_EDD], 0
    mov byte [rs_try], RETRIES
    jmp .next
.give_up:
    pop es
    popad
    stc
    ret

putc:
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    ret

puthex:
    push ax
    shr al, 4
    call .nibble
    pop ax
    and al, 0x0F
.nibble:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 7
    jmp putc

die_status:
    call putc
    mov al, [rs_err]
    call puthex
    jmp hang
die_char:
    call putc
hang:
    cli
.spin:
    hlt
    jmp .spin

rs_left         dw 0
rs_try          db 0
rs_err          db 0xFF

%if ($ - $$) > STAGE2_LBA_OFFSET
  %error "stage1 has grown into the sector where stage2's address is kept"
%endif

times STAGE2_LBA_OFFSET-($-$$) db 0

stage2_lba      dd 1
                dd 0

%if ($ - $$) > 446
  %error "stage1 exceeds 446 bytes — the MBR partition table has no room"
%endif

times 446-($-$$) db 0
times 64 db 0
dw 0xAA55