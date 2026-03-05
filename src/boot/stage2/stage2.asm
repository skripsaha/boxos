[BITS 16]
[ORG 0x8000]
DEFAULT ABS

; Memory map (dynamic layout — addresses computed from actual kernel size):
; 0x0500              - E820 memory map
; 0x7C00              - Stage1 (512 bytes)
; 0x8000              - Stage2 (8192 bytes = 16 sectors)
; 0x9000              - Boot info for kernel (structured, versioned)
; 0x9100              - TagFS superblock buffer (512 bytes)
; 0x9300              - TagFS metadata buffer (512 bytes)
; 0x10000             - Bounce buffer for INT 13h reads (32KB)
; 0x100000            - Kernel run address (1MB, linked address, loaded via Unreal Mode)
; kernel_end + 4KB    - Page tables (32KB: PML4, PDPT, up to 4 PDs) - DYNAMIC
; page_tables + 36KB  - Stack (grows downward) - DYNAMIC

KERNEL_BOUNCE_ADDR    equ 0x10000      ; bounce buffer for INT 13h disk reads
KERNEL_BOUNCE_SEG     equ 0x1000       ; segment value for bounce buffer (0x1000 * 16 = 0x10000)
KERNEL_RUN_ADDR       equ 0x100000     ; linked run address (1MB, must match linker.ld)
KERNEL_MAX_SIZE       equ 0x2000000    ; 32MB sanity limit for kernel binary size

; Page table allocation: PML4(4KB) + PDPT(4KB) + up to 4 PDs(16KB) + spare = 32KB
PAGE_TABLE_SIZE       equ 0x8000       ; 32KB
GUARD_PAGE_SIZE       equ 0x1000       ; 4KB guard page
BOOT_STACK_SIZE       equ 0x10000      ; 64KB boot stack (stack grows downward)

E820_MAP_ADDR         equ 0x500
E820_COUNT_ADDR       equ 0x4FE
E820_SIZE_ADDR        equ 0x4FC
BOOT_INFO_ADDR        equ 0x9000

STAGE2_SIGNATURE      equ 0x2907

; boot_info structure constants (shared contract with kernel)
BOOT_INFO_MAGIC       equ 0x42583031     ; "BX01" — BoxOS boot info v1
BOOT_INFO_VERSION     equ 1

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

    ; Save boot drive number passed by BIOS via stage1 (in DL)
    mov [boot_drive_saved], dl

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
    call enter_unreal_mode
    call detect_memory_e820
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

    ; Temporary stack in former stage1 area (no longer needed, safe)
    mov esp, 0x7C00

    push dword 0
    popf

    mov edi, 0xB8000
    mov al, 'P'
    mov ah, 0x0F
    mov [edi], ax
    mov al, 'M'
    mov [edi+2], ax

    ; Compute dynamic page table and stack placement after kernel
    call compute_dynamic_layout

    ; Validate placement fits within usable E820 RAM
    call validate_dynamic_layout

    ; Switch to final dynamically-placed stack
    mov esp, [dynamic_stack_base]

    call setup_paging
    call enable_long_mode

    jmp 0x18:long_mode_start

; Computes page table and stack addresses from the REAL kernel memory footprint.
; Reads _kernel_end from kernel header at offset +12 (filled by linker, includes BSS).
; Layout (low to high):
;   kernel_end  →  guard  →  page tables (32KB)  →  guard  →  stack space (64KB)
;                                                              ↑ ESP here (top, grows down)
compute_dynamic_layout:
    push eax

    ; Read true kernel_end from kernel header (includes BSS section)
    ; Header: [jmp 2B][KERNEL 6B][version 4B][kernel_end 4B @ offset +12]
    mov eax, [KERNEL_RUN_ADDR + 12]

    ; page-align
    add eax, 0xFFF
    and eax, 0xFFFFF000

    ; page_table_base = kernel_end + guard page
    add eax, GUARD_PAGE_SIZE
    mov [dynamic_pt_base], eax

    ; stack_top = page_table_base + page tables + guard + stack space
    add eax, PAGE_TABLE_SIZE
    add eax, GUARD_PAGE_SIZE
    add eax, BOOT_STACK_SIZE
    mov [dynamic_stack_base], eax

    pop eax
    ret

; Validates that [dynamic_pt_base .. dynamic_stack_base + 64KB] fits in usable E820 RAM.
; Halts with VGA error "NO MEM" if validation fails.
validate_dynamic_layout:
    push eax
    push ebx
    push ecx
    push edx
    push esi

    ; We need usable RAM from pt_base to stack_base (already includes stack space)
    mov edx, [dynamic_stack_base]

    movzx ecx, word [E820_COUNT_ADDR]
    test ecx, ecx
    jz .vdl_fail

    mov esi, E820_MAP_ADDR

.vdl_loop:
    cmp dword [esi+16], 1          ; type == usable?
    jne .vdl_next
    cmp dword [esi+4], 0           ; base_high == 0? (below 4GB)
    jne .vdl_next

    mov eax, [esi]                 ; region_base
    cmp eax, [dynamic_pt_base]
    ja .vdl_next                   ; region starts after our area

    ; region_end = base + length
    mov ebx, eax
    add ebx, [esi+8]
    jc .vdl_found                  ; overflow = huge region, definitely fits
    cmp ebx, edx                   ; region_end >= needed_end?
    jae .vdl_found

.vdl_next:
    add esi, 24
    dec ecx
    jnz .vdl_loop

.vdl_fail:
    ; Fatal: not enough contiguous RAM — display "NO MEM" on VGA
    mov edi, 0xB8000 + 160         ; second VGA line
    mov ah, 0x4F                   ; white on red
    mov al, 'N'
    mov [edi], ax
    mov al, 'O'
    mov [edi+2], ax
    mov al, ' '
    mov [edi+4], ax
    mov al, 'M'
    mov [edi+6], ax
    mov al, 'E'
    mov [edi+8], ax
    mov al, 'M'
    mov [edi+10], ax
    cli
    hlt
    jmp $

.vdl_found:
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

[BITS 64]
long_mode_start:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Use dynamically computed stack
    xor rsp, rsp
    mov esp, [dynamic_stack_base]

    push qword 0
    popf

    mov rdi, 0xB8000
    mov al, 'L'
    mov ah, 0x0F
    mov [rdi+4], ax
    mov al, 'M'
    mov [rdi+6], ax

    ; Kernel was loaded directly to KERNEL_RUN_ADDR by Unreal Mode loader
    mov rax, [KERNEL_RUN_ADDR]
    test rax, rax
    jz .kernel_not_loaded

    ; Write structured boot_info at BOOT_INFO_ADDR
    mov dword [BOOT_INFO_ADDR],      BOOT_INFO_MAGIC    ; +0:  magic
    mov dword [BOOT_INFO_ADDR+4],    BOOT_INFO_VERSION   ; +4:  version
    mov dword [BOOT_INFO_ADDR+8],    E820_MAP_ADDR       ; +8:  e820_map_addr
    mov ax, [E820_COUNT_ADDR]
    mov word  [BOOT_INFO_ADDR+12],   ax                  ; +12: e820_count
    mov word  [BOOT_INFO_ADDR+14],   0                   ; +14: reserved
    mov dword [BOOT_INFO_ADDR+16],   KERNEL_RUN_ADDR     ; +16: kernel_start

    ; kernel_end = KERNEL_RUN_ADDR + kernel_loaded_bytes, aligned to 4KB
    mov eax, [kernel_loaded_bytes]
    add eax, KERNEL_RUN_ADDR
    add eax, 0xFFF
    and eax, 0xFFFFF000
    mov dword [BOOT_INFO_ADDR+20],   eax                 ; +20: kernel_end

    ; boot_drive
    movzx eax, byte [boot_drive_saved]
    mov byte  [BOOT_INFO_ADDR+24],   al                  ; +24: boot_drive
    mov byte  [BOOT_INFO_ADDR+25],   0                   ; +25: reserved
    mov word  [BOOT_INFO_ADDR+26],   0                   ; +26: reserved

    ; Dynamic addresses from compute_dynamic_layout
    mov eax, [dynamic_pt_base]
    mov dword [BOOT_INFO_ADDR+28],   eax                 ; +28: page_table_base
    mov eax, [dynamic_stack_base]
    mov dword [BOOT_INFO_ADDR+32],   eax                 ; +32: stack_base
    mov dword [BOOT_INFO_ADDR+36],   40                  ; +36: total_size

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

; =============================================================
; Enter Unreal Mode (Big Real Mode)
; Sets DS and ES descriptor cache to 4GB limit while remaining
; in Real Mode. Allows 32-bit address overrides (a32 prefix) to
; access memory above 1MB. BIOS interrupts continue to work.
; Requires: A20 line enabled
; =============================================================
enter_unreal_mode:
    cli

    lgdt [unreal_gdt_descriptor]

    ; Enter Protected Mode briefly
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Load DS and ES with flat 4GB data segment (selector 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Return to Real Mode — descriptor caches keep 4GB limit
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    jmp 0x0000:.unreal_flush
.unreal_flush:
    ; Restore segment bases to 0, but caches retain 4GB limit
    xor ax, ax
    mov ds, ax
    mov es, ax

    sti

    mov si, msg_unreal_mode
    call print_string_16
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
    mov dl, [boot_drive_saved]
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
    mov dl, [boot_drive_saved]
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

    mov eax, [es:bx + 12]           ; file size (low 32 bits, offset 12)
    mov [kernel_size_bytes], eax

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

; Load kernel file via bounce buffer + Unreal Mode copy to above 1MB.
; Reads sectors to bounce buffer (0x10000), copies each chunk to
; KERNEL_RUN_ADDR+ using 32-bit Unreal Mode addressing.
; Requires: Unreal Mode active, kernel_start_block/block_count/tagfs_data_start set
; Returns: CF=0 on success (kernel_loaded_bytes set), CF=1 on error
tagfs_load_kernel_file:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    ; sector = tagfs_data_start + (start_block * 8)
    mov eax, [kernel_start_block]
    shl eax, 3              ; * 8 sectors per 4KB block
    add eax, [tagfs_data_start]
    mov [kernel_load_sector], eax

    ; total sectors = block_count * 8
    mov eax, [kernel_block_count]
    shl eax, 3
    mov [kernel_load_sectors], ax

    mov cx, [kernel_load_sectors]
    mov ebx, [kernel_load_sector]
    mov edi, KERNEL_RUN_ADDR          ; destination above 1MB (Unreal Mode)

.load_loop:
    mov ax, 64
    cmp cx, 64
    jae .do_read
    mov ax, cx

.do_read:
    push cx
    push ax

    ; Read chunk to bounce buffer via BIOS INT 13h
    mov [dap_kernel_chunk + 2], ax
    mov word [dap_kernel_chunk + 4], 0x0000
    mov word [dap_kernel_chunk + 6], KERNEL_BOUNCE_SEG
    mov [dap_kernel_chunk + 8], ebx

    mov si, dap_kernel_chunk
    mov ah, 0x42
    mov dl, [boot_drive_saved]
    int 0x13

    pop ax
    pop cx
    jc .load_error

    ; Copy chunk from bounce buffer to destination via Unreal Mode
    push cx
    movzx ecx, ax
    shl ecx, 7              ; ecx = dwords to copy (sectors * 512 / 4)
    mov esi, KERNEL_BOUNCE_ADDR

    cld
    a32 rep movsd            ; DS:ESI -> ES:EDI with 32-bit addressing

    pop cx

    ; Advance LBA, decrement remaining
    movzx eax, ax
    add ebx, eax
    sub cx, ax

    ; Safety: kernel must not exceed max size (sanity check)
    mov eax, edi
    sub eax, KERNEL_RUN_ADDR
    cmp eax, KERNEL_MAX_SIZE
    jae .size_error

    test cx, cx
    jnz .load_loop

    ; Store actual loaded byte count
    mov eax, edi
    sub eax, KERNEL_RUN_ADDR
    mov [kernel_loaded_bytes], eax

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    clc
    ret

.size_error:
    mov si, msg_kernel_too_large
    call print_string_16

.load_error:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
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
    mov dl, [boot_drive_saved]
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

    ; Save size, start_block and block_count from metadata
    mov eax, [es:bx + 12]           ; file size (low 32 bits)
    mov [kernel_size_bytes], eax
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
    mov dl, [boot_drive_saved]
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


[BITS 32]

; Returns ECX = number of 2MB pages to identity-map (min 64, max 2048 = 4GB)
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
    ; Cap at 2048 pages = 4GB (fits in PML4[0] with 4 PDs)
    cmp eax, 2048
    jbe .done
    mov eax, 2048
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

; Sets up identity-mapped page tables at [dynamic_pt_base].
; Layout within allocation: PML4(4KB) + PDPT(4KB) + PD0..PD3(4KB each)
; Maps up to 4GB using 2MB pages, dynamically sized from E820.
setup_paging:
    call calculate_identity_map_size
    ; ECX = total 2MB pages to map (64..2048)

    push ecx
    push ebx

    ; Zero 32KB of page table space at dynamic address
    mov edi, [dynamic_pt_base]
    mov ecx, 8192          ; 32KB / 4 = 8192 dwords
    xor eax, eax
    rep stosd

    pop ebx                ; EBX = total 2MB pages
    pop ecx
    push ecx
    mov ecx, ebx

    ; PML4[0] -> PDPT (base + 0x1000)
    mov esi, [dynamic_pt_base]
    mov eax, esi
    add eax, 0x1000 + 3   ; PDPT address + Present + Writable
    mov [esi], eax
    mov dword [esi + 4], 0x00000000

    ; Calculate number of PDs needed: ceil(pages / 512)
    ; Each PD covers 512 entries × 2MB = 1GB
    mov eax, ecx
    add eax, 511
    shr eax, 9             ; EAX = number of PDs (1..4)
    mov ebx, eax           ; EBX = num_pds

    ; Set up PDPT entries -> PDs (PD0 at +0x2000, PD1 at +0x3000, ...)
    xor edx, edx           ; PDPT entry index
.setup_pdpt:
    mov eax, edx
    shl eax, 12            ; PD offset = index * 4096
    add eax, [dynamic_pt_base]
    add eax, 0x2000
    or eax, 3              ; Present + Writable
    mov edi, [dynamic_pt_base]
    add edi, 0x1000
    lea edi, [edi + edx*8]
    mov [edi], eax
    mov dword [edi+4], 0

    inc edx
    cmp edx, ebx
    jb .setup_pdpt

    ; Fill all PD entries with 2MB identity-mapped pages
    mov edi, [dynamic_pt_base]
    add edi, 0x2000
    mov eax, 0x000083      ; Present, Writable, Page Size (2MB)
    pop ecx                ; ECX = total 2MB pages

.fill_pd:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ret

enable_long_mode:
    mov eax, cr4
    or eax, (1 << 5)      ; PAE
    mov cr4, eax

    mov eax, [dynamic_pt_base]
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

; Unreal Mode GDT: flat 4GB data segment for 32-bit addressing in Real Mode
align 8
unreal_gdt_start:
    dq 0x0000000000000000           ; Null descriptor

    ; 0x08: 16-bit Code (for return to Real Mode)
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A         ; Present, Ring 0, Code, Executable, Readable
    db 0x00         ; 16-bit, byte granularity
    db 0x00

    ; 0x10: 32-bit Data with 4GB limit (the Unreal Mode key segment)
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92         ; Present, Ring 0, Data, Writable
    db 0xCF         ; 4KB granularity, 32-bit, limit = 4GB
    db 0x00

unreal_gdt_end:

align 4
unreal_gdt_descriptor:
    dw unreal_gdt_end - unreal_gdt_start - 1
    dd unreal_gdt_start

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
boot_drive_saved:       db 0

align 4
dynamic_pt_base:        dd 0            ; computed page table base (after kernel)
dynamic_stack_base:     dd 0            ; computed stack base (after page tables)

kernel_file_id:         dw 0
kernel_start_block:     dd 0
kernel_block_count:     dd 0
tagfs_data_start:       dd 0
kernel_load_sector:     dd 0
kernel_load_sectors:    dw 0
kernel_size_bytes:      dd 0            ; actual file size from TagFS metadata
kernel_loaded_bytes:    dd 0            ; total bytes loaded (sectors * 512)

msg_stage2_start      db 'BoxKernel Stage2 Started', 13, 10, 0
msg_a20_enabled       db '[OK] A20 line enabled', 13, 10, 0
msg_detecting_memory  db 'Detecting memory (E820)...', 13, 10, 0
msg_e820_success      db '[OK] E820 memory map created', 13, 10, 0
msg_e820_fail         db '[WARN] E820 failed, using fallback', 13, 10, 0
msg_memory_fallback   db '[OK] Fallback memory detection', 13, 10, 0
msg_memory_error      db '[ERROR] Memory detection failed!', 13, 10, 0
msg_unreal_mode       db '[OK] Unreal Mode (4GB addressing) active', 13, 10, 0
msg_kernel_too_large  db '[ERROR] Kernel exceeds 32MB limit!', 13, 10, 0
msg_loading_tagfs     db 'Loading kernel via TagFS (Unreal Mode)...', 13, 10, 0
msg_kernel_loaded_tagfs db '[OK] Kernel loaded to 0x100000 via Unreal Mode', 13, 10, 0
msg_tagfs_error       db '[ERROR] TagFS superblock read failed!', 13, 10, 0
msg_kernel_not_found  db '[ERROR] Kernel not found in TagFS!', 13, 10, 0
msg_kernel_load_error db '[ERROR] Kernel file load failed!', 13, 10, 0
msg_long_mode_ok      db '[OK] CPU supports 64-bit mode', 13, 10, 0
msg_no_cpuid          db '[ERROR] CPUID not supported!', 13, 10, 0
msg_no_long_mode      db '[ERROR] 64-bit mode not supported!', 13, 10, 0
msg_entering_protected db 'Entering protected mode...', 13, 10, 0
; (page table validation now done dynamically in 32-bit mode via VGA "NO MEM" on failure)
msg_kernel_tag_not_found  db '[WARN] Kernel tag not found, searching by header...', 13, 10, 0
msg_kernel_loaded_header  db '[OK] Kernel loaded via header scan', 13, 10, 0
