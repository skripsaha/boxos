[BITS 16]
[ORG 0x8000]
DEFAULT ABS

; Memory map:
; 0x0500      - E820 memory map
; 0x7C00      - Stage1 (512 bytes)
; 0x8000      - Stage2 (8192 bytes = 16 sectors)
; 0x9000      - Boot info for kernel
; 0x9100      - TagFS superblock buffer (512 bytes)
; 0x9300      - TagFS metadata buffer (512 bytes)
; 0x10000     - Kernel temporary load address (real mode, 512KB)
; 0x90000     - Kernel temp end
; 0x100000    - Kernel run address (1MB, linked address)
; 0x180000    - Kernel run end
; 0x820000    - Page tables (16KB: PML4, PDPT, PD) - E820 validated
; 0x900000    - Stack (grows downward)

KERNEL_LOAD_ADDR      equ 0x10000      ; real mode load (below 1MB)
KERNEL_RUN_ADDR       equ 0x100000     ; linked run address (1MB)
KERNEL_SECTOR_START   equ 17
KERNEL_SECTOR_COUNT   equ 1024         ; 512KB
KERNEL_SIZE_BYTES     equ 524288       ; 1024 * 512
KERNEL_END_ADDR       equ 0x180000     ; KERNEL_RUN_ADDR + KERNEL_SIZE_BYTES

; 0x820000: well above kernel end (0x90000), 7.5MB gap, E820-validated, page-aligned, 16KB contiguous
PAGE_TABLE_BASE       equ 0x820000
E820_MAP_ADDR         equ 0x500
E820_COUNT_ADDR       equ 0x4FE
E820_SIZE_ADDR        equ 0x4FC
STACK_BASE            equ 0x900000
BOOT_INFO_ADDR        equ 0x9000

BOOT_DISK             equ 0x80
STAGE2_SIGNATURE      equ 0x2907

TAGFS_SUPERBLOCK_SECTOR equ 1034
TAGFS_METADATA_START    equ 1035
TAGFS_MAGIC             equ 0x54414746  ; "TAGF"
TAGFS_METADATA_MAGIC    equ 0x544D4554  ; "TMET"
TAGFS_FILE_ACTIVE       equ 1

TAGFS_SUPERBLOCK_ADDR   equ 0x9100
TAGFS_METADATA_ADDR     equ 0x9300

KERNEL_HDR_MAGIC        equ 0x4E52454B  ; "KERN" little-endian
KERNEL_HDR_MAGIC_HI     equ 0x4C45      ; "EL" little-endian

dw STAGE2_SIGNATURE

start_stage2:
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ss, ax
    mov sp, 0x7C00

    sti

    mov si, msg_stage2_start
    call print_string_16

    call enable_a20_enhanced
    call detect_memory_e820
    call validate_page_table_location
    call load_kernel_tagfs
    call check_long_mode_support

    mov si, msg_entering_protected
    call print_string_16

    cli

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode_start

[BITS 32]
protected_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, STACK_BASE

    push dword 0
    popf

    mov edi, 0xB8000
    mov al, 'P'
    mov ah, 0x0F
    mov [edi], ax
    mov al, 'M'
    mov [edi+2], ax

    call setup_paging_fixed
    call enable_long_mode_fixed

    jmp 0x18:long_mode_start

[BITS 64]
long_mode_start:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, STACK_BASE

    push qword 0
    popf

    mov rdi, 0xB8000
    mov al, 'L'
    mov ah, 0x0F
    mov [rdi+4], ax
    mov al, 'M'
    mov [rdi+6], ax

    mov rdi, E820_MAP_ADDR
    movzx rsi, word [E820_COUNT_ADDR]

    mov rax, [KERNEL_LOAD_ADDR]
    test rax, rax
    jz .kernel_not_loaded

    ; Copy kernel from temp load address (0x10000) to linked run address (0x100000)
    mov rsi, KERNEL_LOAD_ADDR
    mov rdi, KERNEL_RUN_ADDR
    mov rcx, KERNEL_SIZE_BYTES / 8
    rep movsq

    mov [BOOT_INFO_ADDR], dword E820_MAP_ADDR
    mov ax, [E820_COUNT_ADDR]
    mov [BOOT_INFO_ADDR+4], ax
    mov [BOOT_INFO_ADDR+8], dword KERNEL_RUN_ADDR
    mov [BOOT_INFO_ADDR+12], dword KERNEL_END_ADDR

    ; Zero region after kernel end before jumping (BSS pre-clear)
    mov rdi, KERNEL_END_ADDR
    mov rcx, 0x100000
    xor rax, rax
    rep stosb

    jmp KERNEL_RUN_ADDR

.kernel_not_loaded:
    mov rdi, 0xB8000
    mov al, 'N'
    mov ah, 0x04
    mov [rdi+8], ax
    mov al, 'K'
    mov [rdi+10], ax

.halt:
    cli
    hlt
    jmp $

[BITS 16]

check_long_mode_support:
    push eax
    push ebx
    push ecx
    push edx

    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x00200000
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_cpuid

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; LM bit
    jz .no_long_mode

    mov si, msg_long_mode_ok
    call print_string_16

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

.no_cpuid:
    mov si, msg_no_cpuid
    call print_string_16
    jmp .halt

.no_long_mode:
    mov si, msg_no_long_mode
    call print_string_16
    jmp .halt

.halt:
    cli
    hlt
    jmp $

print_string_16:
    push ax
    push bx
    push si

    mov ah, 0x0E
    mov bh, 0

.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop

.done:
    pop si
    pop bx
    pop ax
    ret

wait_key:
    push ax
    mov ah, 0x00
    int 0x16
    pop ax
    ret

enable_a20_enhanced:
    push ax
    push cx

    call test_a20
    jnc .a20_done

    ; Method 1: BIOS
    mov ax, 0x2401
    int 0x15
    call test_a20
    jnc .a20_done

    ; Method 2: Keyboard controller
    call a20_wait
    mov al, 0xAD
    out 0x64, al

    call a20_wait
    mov al, 0xD0
    out 0x64, al

    call a20_wait2
    in al, 0x60
    push ax

    call a20_wait
    mov al, 0xD1
    out 0x64, al

    call a20_wait
    pop ax
    or al, 2
    out 0x60, al

    call a20_wait
    mov al, 0xAE
    out 0x64, al

    call a20_wait
    call test_a20
    jnc .a20_done

    ; Method 3: Fast A20 (port 0x92)
    in al, 0x92
    test al, 2
    jnz .a20_done
    or al, 2
    and al, 0xFE
    out 0x92, al

.a20_done:
    pop cx
    pop ax

    mov si, msg_a20_enabled
    call print_string_16
    ret

; Returns CF=0 if A20 enabled, CF=1 if disabled
test_a20:
    push ax
    push bx
    push es
    push ds

    xor ax, ax
    mov es, ax
    mov ds, ax

    mov bx, 0x7DFE
    mov al, [es:bx]
    push ax

    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x7E0E
    mov ah, [es:bx]
    push ax

    mov byte [es:bx], 0x00
    xor ax, ax
    mov es, ax
    mov byte [es:0x7DFE], 0xFF

    mov ax, 0xFFFF
    mov es, ax
    cmp byte [es:bx], 0xFF

    pop ax
    mov [es:bx], ah
    xor ax, ax
    mov es, ax
    pop ax
    mov [es:0x7DFE], al

    pop ds
    pop es
    pop bx
    pop ax

    je .a20_disabled
    clc
    ret
.a20_disabled:
    stc
    ret

a20_wait:
    in al, 0x64
    test al, 2
    jnz a20_wait
    ret

a20_wait2:
    in al, 0x64
    test al, 1
    jz a20_wait2
    ret

load_kernel_tagfs:
    mov si, msg_loading_tagfs
    call print_string_16

    call tagfs_read_superblock
    jc .tagfs_error

    call tagfs_find_kernel
    jc .try_header_fallback

    call tagfs_load_kernel_file
    jc .load_error

    mov si, msg_kernel_loaded_tagfs
    call print_string_16
    ret

.try_header_fallback:
    mov si, msg_kernel_tag_not_found
    call print_string_16

    call tagfs_find_kernel_by_header
    jc .kernel_not_found

    call tagfs_load_kernel_file
    jc .load_error

    mov si, msg_kernel_loaded_header
    call print_string_16
    ret

.tagfs_error:
    mov si, msg_tagfs_error
    call print_string_16
    jmp .halt

.kernel_not_found:
    mov si, msg_kernel_not_found
    call print_string_16
    jmp .halt

.load_error:
    mov si, msg_kernel_load_error
    call print_string_16
    jmp .halt

.halt:
    call wait_key
    cli
    hlt
    jmp $

; Returns: CF=0 on success, CF=1 on error
tagfs_read_superblock:
    push ax
    push dx
    push si

    mov si, dap_tagfs_superblock
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .error

    mov ax, 0x910
    mov es, ax
    xor bx, bx
    mov eax, [es:bx]
    cmp eax, TAGFS_MAGIC
    jne .error

    xor ax, ax
    mov es, ax

    clc
    pop si
    pop dx
    pop ax
    ret

.error:
    xor ax, ax
    mov es, ax
    stc
    pop si
    pop dx
    pop ax
    ret

; Returns: CF=0 on success (kernel_file_id, start_block, block_count set), CF=1 on error
tagfs_find_kernel:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    mov cx, 64

.scan_loop:
    push cx

    mov ax, 64
    sub ax, cx
    add ax, TAGFS_METADATA_START
    mov [dap_tagfs_metadata + 8], ax

    mov si, dap_tagfs_metadata
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .scan_next

    mov ax, 0x930
    mov es, ax
    xor bx, bx
    mov eax, [es:bx]
    cmp eax, TAGFS_METADATA_MAGIC
    jne .scan_next

    mov al, [es:bx + 8]
    test al, TAGFS_FILE_ACTIVE
    jz .scan_next

    mov di, 80              ; Tags array offset

    mov bx, 16

.tag_loop:
    ; Bounds check: entire tag (24 bytes) must fit in 512-byte sector
    mov ax, di
    add ax, 24
    cmp ax, 512
    jae .next_tag

    ; tag.key starts at offset di+1
    lea si, [di + 1]

    mov ax, es
    push ds
    mov ds, ax

    ; Match key = "kernel" (system tag, no value check needed)
    mov al, [si]
    cmp al, 'k'
    jne .next_tag_pop
    mov al, [si+1]
    cmp al, 'e'
    jne .next_tag_pop
    mov al, [si+2]
    cmp al, 'r'
    jne .next_tag_pop
    mov al, [si+3]
    cmp al, 'n'
    jne .next_tag_pop
    mov al, [si+4]
    cmp al, 'e'
    jne .next_tag_pop
    mov al, [si+5]
    cmp al, 'l'
    jne .next_tag_pop
    mov al, [si+6]
    test al, al
    jnz .next_tag_pop

    pop ds
    xor bx, bx
    mov eax, [es:bx + 4]
    mov [kernel_file_id], ax

    mov eax, [es:bx + 20]
    mov [kernel_start_block], eax

    mov eax, [es:bx + 24]
    mov [kernel_block_count], eax

    mov ax, 0x910
    push es
    mov es, ax
    xor bx, bx
    mov eax, [es:bx + 32]
    mov [tagfs_data_start], eax
    pop es

    xor ax, ax
    mov es, ax

    pop cx
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.next_tag_pop:
    pop ds

.next_tag:
    add di, 24
    dec bx
    jnz .tag_loop

.scan_next:
    pop cx
    dec cx
    jnz .scan_loop

    xor ax, ax
    mov es, ax
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; Returns: CF=0 on success, CF=1 on error
tagfs_load_kernel_file:
    push ax
    push bx
    push cx
    push dx
    push si

    ; sector = tagfs_data_start + (start_block * 8)
    mov eax, [kernel_start_block]
    shl eax, 3              ; * 8 sectors per 4KB block
    add eax, [tagfs_data_start]
    mov [kernel_load_sector], eax

    mov eax, [kernel_block_count]
    shl eax, 3
    mov [kernel_load_sectors], ax

    mov cx, [kernel_load_sectors]
    mov ebx, [kernel_load_sector]
    mov di, 0x1000

.load_loop:
    mov ax, 64
    cmp cx, 64
    jae .load_chunk
    mov ax, cx

.load_chunk:
    push cx
    push ax

    mov [dap_kernel_chunk + 2], ax
    mov [dap_kernel_chunk + 4], word 0x0000
    mov [dap_kernel_chunk + 6], di
    mov [dap_kernel_chunk + 8], ebx

    mov si, dap_kernel_chunk
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13

    pop ax
    pop cx

    jc .load_error

    movzx eax, ax
    add ebx, eax
    sub cx, ax

    shl ax, 5               ; sectors * 32 = segment increment
    add di, ax

    test cx, cx
    jnz .load_loop

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.load_error:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; Fallback: find kernel by scanning file data for KERNEL header magic
; Iterates metadata entries, reads first data sector of each active file,
; checks for "KERNEL" magic at offset 2.
; Returns: CF=0 on success (kernel_start_block, kernel_block_count, tagfs_data_start set), CF=1 on error
tagfs_find_kernel_by_header:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; Get data_start_sector from superblock (already at 0x9100)
    mov ax, 0x910
    mov es, ax
    xor bx, bx
    mov eax, [es:bx + 32]          ; data_start_sector
    mov [tagfs_data_start], eax
    xor ax, ax
    mov es, ax

    mov cx, 64                      ; scan up to 64 metadata entries

.hdr_scan_loop:
    push cx

    ; Calculate metadata sector number
    mov ax, 64
    sub ax, cx
    add ax, TAGFS_METADATA_START
    mov [dap_tagfs_metadata + 8], ax
    mov dword [dap_tagfs_metadata + 10], 0

    ; Read metadata sector
    mov si, dap_tagfs_metadata
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .hdr_scan_next

    ; Check metadata magic
    mov ax, 0x930
    mov es, ax
    xor bx, bx
    mov eax, [es:bx]
    cmp eax, TAGFS_METADATA_MAGIC
    jne .hdr_scan_next_clean

    ; Check file is active
    mov al, [es:bx + 8]
    test al, TAGFS_FILE_ACTIVE
    jz .hdr_scan_next_clean

    ; Save start_block and block_count from metadata
    mov eax, [es:bx + 20]
    mov [kernel_start_block], eax
    mov eax, [es:bx + 24]
    mov [kernel_block_count], eax

    ; Calculate data sector: data_start + start_block * 8
    mov eax, [kernel_start_block]
    shl eax, 3
    add eax, [tagfs_data_start]

    ; Restore ES before disk read
    xor bx, bx
    mov es, bx

    ; Read first sector of this file's data into 0x9300 buffer
    mov [dap_tagfs_metadata + 8], eax
    mov dword [dap_tagfs_metadata + 10], 0
    mov si, dap_tagfs_metadata
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .hdr_scan_next

    ; Check for KERNEL magic at offset 2
    mov ax, 0x930
    mov es, ax
    xor bx, bx
    cmp dword [es:bx + 2], KERNEL_HDR_MAGIC
    jne .hdr_scan_next_clean
    cmp word [es:bx + 6], KERNEL_HDR_MAGIC_HI
    jne .hdr_scan_next_clean

    ; Found kernel by header!
    xor ax, ax
    mov es, ax

    pop cx
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.hdr_scan_next_clean:
    xor ax, ax
    mov es, ax

.hdr_scan_next:
    pop cx
    dec cx
    jnz .hdr_scan_loop

    xor ax, ax
    mov es, ax
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; Fallback loader (kept for reference, not called in normal boot)
load_kernel_simple_old:
    mov si, msg_loading_kernel
    call print_string_16

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap1
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap2
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap3
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap4
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap5
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap6
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap7
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap8
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap9
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap10
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap11
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap12
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap13
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap14
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap15
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    mov si, dap16
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

.check_kernel:
    mov ax, 0x1000
    mov es, ax
    mov bx, 0x0000
    mov eax, [es:bx]
    test eax, eax
    jz .empty_kernel

    xor ax, ax
    mov es, ax

    mov si, msg_kernel_loaded
    call print_string_16
    ret

.empty_kernel:
    xor ax, ax
    mov es, ax
    mov si, msg_kernel_empty
    call print_string_16
    ret

.disk_error:
    mov si, msg_disk_error
    call print_string_16
    call wait_key
    cli
    hlt

detect_memory_e820:
    mov si, msg_detecting_memory
    call print_string_16

    mov ax, 0x50
    mov es, ax
    xor di, di
    mov cx, 1024
    xor ax, ax
    rep stosw

    xor ebx, ebx
    mov edx, 0x534D4150    ; 'SMAP'
    mov ax, 0x50
    mov es, ax
    xor di, di
    xor bp, bp

.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .e820_fail

    cmp eax, 0x534D4150
    jne .e820_fail

    cmp ecx, 20
    jl .skip_entry

    inc bp
    add di, 24

.skip_entry:
    test ebx, ebx
    jnz .e820_loop

    xor ax, ax
    mov es, ax

    mov [E820_COUNT_ADDR], bp

    ; bp * 24: (bp << 4) + (bp << 3)
    mov ax, bp
    shl ax, 4
    mov cx, bp
    shl cx, 3
    add ax, cx
    mov [E820_SIZE_ADDR], ax

    mov si, msg_e820_success
    call print_string_16
    ret

.e820_fail:
    mov si, msg_e820_fail
    call print_string_16

    ; Fallback: create minimal memory map
    mov ax, 0x50
    mov es, ax
    xor di, di

    mov dword [es:di], 0x00000000
    mov dword [es:di+4], 0x00000000
    mov dword [es:di+8], 0x0009FC00    ; 640KB
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0
    add di, 24

    mov ah, 0x88
    int 0x15
    jc .memory_fail

    mov dword [es:di], 0x00100000
    mov dword [es:di+4], 0x00000000
    movzx eax, ax
    shl eax, 10
    mov [es:di+8], eax
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0

    xor ax, ax
    mov es, ax

    mov word [E820_COUNT_ADDR], 2
    mov word [E820_SIZE_ADDR], 48

    mov si, msg_memory_fallback
    call print_string_16
    ret

.memory_fail:
    xor ax, ax
    mov es, ax
    mov si, msg_memory_error
    call print_string_16
    ret

; Validates PAGE_TABLE_BASE (0x820000-0x823FFF) is in usable RAM via E820
; Halts if not found
validate_page_table_location:
    push ax
    push bx
    push cx
    push si
    push di
    push es

    mov cx, [E820_COUNT_ADDR]
    test cx, cx
    jz .validation_failed

    mov ax, 0x50
    mov es, ax
    xor di, di

.check_loop:
    mov eax, [es:di]
    mov ebx, [es:di+4]

    mov esi, [es:di+8]
    mov edx, [es:di+12]

    add esi, eax
    adc edx, ebx

    mov eax, [es:di+16]
    cmp eax, 1
    jne .next_entry

    mov eax, [es:di]
    mov ebx, [es:di+4]

    cmp ebx, 0x00
    ja .next_entry
    jb .check_end
    cmp eax, 0x820000
    ja .next_entry

.check_end:
    cmp edx, 0x00
    ja .found
    jb .next_entry
    cmp esi, 0x824000
    jae .found

.next_entry:
    add di, 24
    dec cx
    jnz .check_loop

    jmp .validation_failed

.found:
    xor ax, ax
    mov es, ax

    mov si, msg_page_table_safe
    call print_string_16

    pop es
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

.validation_failed:
    xor ax, ax
    mov es, ax

    mov si, msg_page_table_unsafe
    call print_string_16
    call wait_key
    cli
    hlt
    jmp $

[BITS 32]

; Returns ECX = number of 2MB pages to identity-map (min 64, max 512)
calculate_identity_map_size:
    push eax
    push ebx
    push edx
    push esi
    push edi

    movzx ecx, word [E820_COUNT_ADDR]
    test ecx, ecx
    jz .use_default

    xor edx, edx
    xor ebx, ebx

    mov esi, E820_MAP_ADDR

.e820_loop:
    mov eax, [esi + 16]
    cmp eax, 1
    jne .skip_entry

    mov eax, [esi]
    mov edi, [esi + 4]
    add eax, [esi + 8]
    adc edi, [esi + 12]

    cmp edi, edx
    ja .update_max
    jb .skip_entry
    cmp eax, ebx
    jbe .skip_entry

.update_max:
    mov ebx, eax
    mov edx, edi

.skip_entry:
    add esi, 24
    loop .e820_loop

    ; Convert max_address (EDX:EBX) to 2MB page count
    mov eax, edx
    shl eax, 11
    shr ebx, 21
    add eax, ebx
    inc eax

    cmp eax, 64
    jae .check_max
    mov eax, 64
    jmp .done

.check_max:
    cmp eax, 512
    jbe .done
    mov eax, 512
    jmp .done

.use_default:
    mov eax, 64

.done:
    mov ecx, eax

    pop edi
    pop esi
    pop edx
    pop ebx
    pop eax
    ret

setup_paging_fixed:
    call calculate_identity_map_size

    push ecx

    mov edi, PAGE_TABLE_BASE
    mov ecx, 4096
    xor eax, eax
    rep stosd

    ; PML4[0] -> PDPT
    mov dword [PAGE_TABLE_BASE], PAGE_TABLE_BASE + 0x1000 + 3
    mov dword [PAGE_TABLE_BASE + 4], 0x00000000

    ; PDPT[0] -> PD
    mov dword [PAGE_TABLE_BASE + 0x1000], PAGE_TABLE_BASE + 0x2000 + 3
    mov dword [PAGE_TABLE_BASE + 0x1004], 0x00000000

    ; PD: identity map 2MB pages
    mov edi, PAGE_TABLE_BASE
    add edi, 0x2000
    mov eax, 0x000083     ; Present, Writable, Page Size (2MB)
    pop ecx

.fill_pd:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ret

enable_long_mode_fixed:
    mov eax, cr4
    or eax, (1 << 5)      ; PAE
    mov cr4, eax

    mov eax, PAGE_TABLE_BASE
    mov cr3, eax

    mov ecx, 0xC0000080   ; EFER MSR
    rdmsr
    or eax, (1 << 8)      ; LME
    or eax, (1 << 11)     ; NXE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)     ; PG
    mov cr0, eax

    ret

align 8
gdt_start:
    dq 0x0000000000000000           ; Null

    ; 0x08: 32-bit Code
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A         ; Present, Ring 0, Code, Executable, Readable
    db 0xCF         ; 4KB granularity, 32-bit
    db 0x00

    ; 0x10: 32-bit Data
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92         ; Present, Ring 0, Data, Writable
    db 0xCF
    db 0x00

    ; 0x18: 64-bit Code
    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x9A         ; Present, Ring 0, Code, Executable, Readable
    db 0x20         ; Long mode (L=1)
    db 0x00

    ; 0x20: 64-bit Data
    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x92         ; Present, Ring 0, Data, Writable
    db 0x00
    db 0x00

gdt_end:

align 4
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; 16 DAP chunks, 64 sectors each = 1024 sectors = 512KB
align 4
dap1:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x1000           ; 0x10000 physical
    dq 17

align 4
dap2:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x1800           ; 0x18000
    dq 81

align 4
dap3:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x2000
    dq 145

align 4
dap4:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x2800
    dq 209

align 4
dap5:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x3000
    dq 273

align 4
dap6:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x3800
    dq 337

align 4
dap7:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x4000
    dq 401

align 4
dap8:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x4800
    dq 465

align 4
dap9:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x5000
    dq 529

align 4
dap10:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x5800
    dq 593

align 4
dap11:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x6000
    dq 657

align 4
dap12:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x6800
    dq 721

align 4
dap13:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x7000
    dq 785

align 4
dap14:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x7800
    dq 849

align 4
dap15:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x8000
    dq 913

align 4
dap16:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x8800
    dq 977

align 4
dap_tagfs_superblock:
    db 0x10, 0
    dw 1
    dw 0x0000
    dw 0x0910           ; 0x9100 physical
    dq TAGFS_SUPERBLOCK_SECTOR

align 4
dap_tagfs_metadata:
    db 0x10, 0
    dw 1
    dw 0x0000
    dw 0x0930           ; 0x9300 physical
    dq TAGFS_METADATA_START

align 4
dap_kernel_chunk:
    db 0x10, 0
    dw 64               ; updated dynamically
    dw 0x0000           ; updated dynamically
    dw 0x1000           ; updated dynamically
    dq 0                ; updated dynamically

align 4
kernel_file_id:         dw 0
kernel_start_block:     dd 0
kernel_block_count:     dd 0
tagfs_data_start:       dd 0
kernel_load_sector:     dd 0
kernel_load_sectors:    dw 0

msg_stage2_start      db 'BoxKernel Stage2 Started', 13, 10, 0
msg_a20_enabled       db '[OK] A20 line enabled', 13, 10, 0
msg_detecting_memory  db 'Detecting memory (E820)...', 13, 10, 0
msg_e820_success      db '[OK] E820 memory map created', 13, 10, 0
msg_e820_fail         db '[WARN] E820 failed, using fallback', 13, 10, 0
msg_memory_fallback   db '[OK] Fallback memory detection', 13, 10, 0
msg_memory_error      db '[ERROR] Memory detection failed!', 13, 10, 0
msg_loading_kernel    db 'Loading kernel (1024 sectors = 512KB)...', 13, 10, 0
msg_kernel_loaded     db '[OK] Kernel loaded (512KB)', 13, 10, 0
msg_loading_tagfs     db 'Loading kernel via TagFS...', 13, 10, 0
msg_kernel_loaded_tagfs db '[OK] Kernel loaded via TagFS', 13, 10, 0
msg_tagfs_error       db '[ERROR] TagFS superblock read failed!', 13, 10, 0
msg_kernel_not_found  db '[ERROR] Kernel not found in TagFS!', 13, 10, 0
msg_kernel_load_error db '[ERROR] Kernel file load failed!', 13, 10, 0
msg_kernel_empty      db '[WARN] Kernel appears empty', 13, 10, 0
msg_disk_error        db '[ERROR] Disk read failed!', 13, 10, 0
msg_long_mode_ok      db '[OK] CPU supports 64-bit mode', 13, 10, 0
msg_no_cpuid          db '[ERROR] CPUID not supported!', 13, 10, 0
msg_no_long_mode      db '[ERROR] 64-bit mode not supported!', 13, 10, 0
msg_entering_protected db 'Entering protected mode...', 13, 10, 0
msg_page_table_safe   db '[OK] Page table location validated (0x820000)', 13, 10, 0
msg_page_table_unsafe db '[ERROR] Page tables at 0x820000 in unusable memory! Need 8MB+ RAM', 13, 10, 0
msg_kernel_tag_not_found  db '[WARN] Kernel tag not found, searching by header...', 13, 10, 0
msg_kernel_loaded_header  db '[OK] Kernel loaded via header scan', 13, 10, 0
