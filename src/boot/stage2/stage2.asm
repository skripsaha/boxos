[BITS 16]
[ORG 0x8000]
DEFAULT ABS

; ===================================================================
; STAGE2 - Advanced Bootloader (8192 bytes, 16 sectors)
; ===================================================================
; Responsibilities:
;   - Enable A20 line
;   - Detect memory (E820)
;   - Load kernel from disk
;   - Setup paging for long mode
;   - Enter 64-bit long mode
;   - Jump to kernel
; ===================================================================

; === MEMORY MAP ===
; Low Memory Region (<1MB):
; 0x0500      - E820 memory map (safe low memory, up to 2KB)
; 0x7C00      - Stage1 (512 bytes)
; 0x8000      - Stage2 (8192 bytes = 16 sectors) - THIS CODE
; 0x9000      - Boot info for kernel (256 bytes)
; 0x9100      - TagFS superblock buffer (512 bytes)
; 0x9300      - TagFS metadata buffer (512 bytes)
;
; High Memory Region (>1MB):
; 0x10000     - Kernel load address (524288 bytes = 1024 sectors = 512KB)
; 0x90000     - Kernel end (0x10000 + 0x80000)
;               *** 7.5MB SAFETY GAP ***
; 0x820000    - Page tables (16KB: PML4, PDPT, PD) - E820 VALIDATED
; 0x900000    - Stack for 32/64-bit modes (grows downward)
;
; NOTES:
; - Page table location verified safe via E820 scan (must be in usable RAM)
; - Kernel BSS is dynamically allocated after kernel_end by PMM
; - Minimum system RAM requirement: 8MB+ (enforced by validation)

; === CONSTANTS ===
KERNEL_LOAD_ADDR      equ 0x10000
KERNEL_SECTOR_START   equ 17
KERNEL_SECTOR_COUNT   equ 1024         ; Load 1024 sectors (512KB)
KERNEL_SIZE_BYTES     equ 524288       ; 1024 * 512
KERNEL_END_ADDR       equ 0x90000      ; End of kernel in memory (0x10000 + 0x80000)

; Page table base address at 8MB+ mark
; SAFETY: 0x820000 chosen for critical reasons:
;   1. Well above kernel end (0x90000), provides 7.5MB safety margin
;   2. Avoids low memory fragmentation (<1MB used for bootloader data)
;   3. Validated via E820 to ensure location is in usable RAM region
;   4. Must be page-aligned (4KB boundary) for CR3 register
;   5. Requires 16KB contiguous space: PML4(4KB) + PDPT(4KB) + PD(8KB)
; REQUIREMENT: System must have 8MB+ RAM (enforced by validation function)
PAGE_TABLE_BASE       equ 0x820000
E820_MAP_ADDR         equ 0x500         ; Low memory (safe after BIOS data area)
E820_COUNT_ADDR       equ 0x4FE         ; Just before E820 map
E820_SIZE_ADDR        equ 0x4FC         ; Just before count
STACK_BASE            equ 0x900000      ; Safe stack location (well above kernel and page tables)
BOOT_INFO_ADDR        equ 0x9000        ; After Stage2

BOOT_DISK             equ 0x80
STAGE2_SIGNATURE      equ 0x2907

; TagFS constants
TAGFS_SUPERBLOCK_SECTOR equ 1034
TAGFS_METADATA_START    equ 1035
TAGFS_MAGIC             equ 0x54414746  ; "TAGF"
TAGFS_METADATA_MAGIC    equ 0x544D4554  ; "TMET"
TAGFS_FILE_ACTIVE       equ 1

; Memory buffers for TagFS (in low memory < 1MB)
TAGFS_SUPERBLOCK_ADDR   equ 0x9100      ; 512 bytes
TAGFS_METADATA_ADDR     equ 0x9300      ; 512 bytes

; Signature for Stage1 verification
dw STAGE2_SIGNATURE

start_stage2:
    ; Disable interrupts during initialization
    cli

    ; Clear direction flag
    cld

    ; Setup segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Setup stack (16-byte aligned for future BIOS calls)
    mov ss, ax
    mov sp, 0x7C00          ; Stack below Stage1, grows downward

    ; Re-enable interrupts for BIOS calls
    sti

    ; Print startup message
    mov si, msg_stage2_start
    call print_string_16

    ; Enable A20 line
    call enable_a20_enhanced

    ; Detect memory with E820
    call detect_memory_e820

    ; Validate page table location is safe (E820 check)
    call validate_page_table_location

    ; Load kernel via TagFS
    call load_kernel_tagfs

    ; Check CPU compatibility (long mode support)
    call check_long_mode_support

    ; Entering protected mode
    mov si, msg_entering_protected
    call print_string_16

    ; Disable interrupts before mode switch
    cli

    ; Load GDT
    lgdt [gdt_descriptor]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to flush pipeline and load CS
    jmp 0x08:protected_mode_start

[BITS 32]
protected_mode_start:
    ; Setup segments in 32-bit mode
    mov ax, 0x10        ; Data segment selector (GDT entry 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack (16-byte aligned)
    mov esp, STACK_BASE
    
    ; Очистка EFLAGS
    push dword 0
    popf
    
    ; Сообщение о входе в защищенный режим
    mov edi, 0xB8000
    mov al, 'P'
    mov ah, 0x0F
    mov [edi], ax
    mov al, 'M'
    mov [edi+2], ax
    
    ; Инициализация страничной адресации
    call setup_paging_fixed
    
    ; Переход в long mode
    call enable_long_mode_fixed
    
    ; Far jump в 64-bit режим
    jmp 0x18:long_mode_start

[BITS 64]
long_mode_start:
    ; Setup segments in 64-bit mode
    mov ax, 0x20        ; Data segment selector (GDT entry 4)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack for 64-bit mode (16-byte aligned)
    mov rsp, STACK_BASE
    
    ; Очистка RFLAGS
    push qword 0
    popf
    
    ; Сообщение о входе в long mode
    mov rdi, 0xB8000
    mov al, 'L'
    mov ah, 0x0F
    mov [rdi+4], ax
    mov al, 'M'
    mov [rdi+6], ax
    
    ; Prepare parameters for kernel
    mov rdi, E820_MAP_ADDR              ; E820 memory map address
    movzx rsi, word [E820_COUNT_ADDR]   ; E820 entry count

    ; Verify kernel was loaded (check first 8 bytes)
    mov rax, [KERNEL_LOAD_ADDR]
    test rax, rax
    jz .kernel_not_loaded

    ; Save boot info for kernel (at BOOT_INFO_ADDR)
    mov [BOOT_INFO_ADDR], dword E820_MAP_ADDR   ; E820 map address
    mov ax, [E820_COUNT_ADDR]
    mov [BOOT_INFO_ADDR+4], ax                  ; E820 entry count
    mov [BOOT_INFO_ADDR+8], dword KERNEL_LOAD_ADDR ; Kernel load address
    mov [BOOT_INFO_ADDR+12], dword KERNEL_END_ADDR ; Kernel end address

    ; Zero kernel BSS section before jumping to it
    ; CRITICAL: Global variables must be zero-initialized
    mov rdi, KERNEL_END_ADDR                    ; Start from kernel end
    mov rcx, 0x100000                           ; Zero until 1MB mark (conservative)
    xor rax, rax
    rep stosb

    ; Now safe to jump to kernel with zeroed BSS
    jmp KERNEL_LOAD_ADDR
    
.kernel_not_loaded:
    ; Сообщение об ошибке
    mov rdi, 0xB8000
    mov al, 'N'
    mov ah, 0x04        ; Красный цвет
    mov [rdi+8], ax
    mov al, 'K'
    mov [rdi+10], ax
    jmp .halt
    
.halt:
    cli
    hlt
    jmp $

[BITS 16]
; ===== 16-BIT MODE FUNCTIONS =====

; Check if CPU supports long mode (64-bit)
check_long_mode_support:
    push eax
    push ebx
    push ecx
    push edx

    ; Check if CPUID is supported
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x00200000         ; Flip ID bit
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_cpuid                ; ID bit didn't change, no CPUID

    ; Check if extended CPUID functions are available
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode            ; Extended functions not available

    ; Check for long mode support
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; LM bit (bit 29)
    jz .no_long_mode

    ; Long mode is supported
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

; Улучшенная функция включения A20
enable_a20_enhanced:
    push ax
    push cx
    
    ; Сначала проверим, не включена ли A20 уже
    call test_a20
    jnc .a20_done
    
    ; Метод 1: BIOS function
    mov ax, 0x2401
    int 0x15
    call test_a20
    jnc .a20_done
    
    ; Метод 2: Keyboard controller
    call a20_wait
    mov al, 0xAD        ; Disable keyboard
    out 0x64, al
    
    call a20_wait
    mov al, 0xD0        ; Read output port
    out 0x64, al
    
    call a20_wait2
    in al, 0x60
    push ax
    
    call a20_wait
    mov al, 0xD1        ; Write output port
    out 0x64, al
    
    call a20_wait
    pop ax
    or al, 2            ; Set A20 bit
    out 0x60, al
    
    call a20_wait
    mov al, 0xAE        ; Enable keyboard
    out 0x64, al
    
    call a20_wait
    call test_a20
    jnc .a20_done
    
    ; Метод 3: Fast A20 (port 0x92)
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

; Тест A20 линии
test_a20:
    push ax
    push bx
    push es
    push ds
    
    ; Устанавливаем сегменты для теста
    xor ax, ax
    mov es, ax
    mov ds, ax
    
    mov bx, 0x7DFE      ; Адрес в первом мегабайте
    mov al, [es:bx]     ; Сохраняем оригинальное значение
    push ax
    
    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x7E0E      ; Соответствующий адрес во втором мегабайте
    mov ah, [es:bx]     ; Сохраняем оригинальное значение
    push ax
    
    ; Записываем тестовые значения
    mov byte [es:bx], 0x00
    xor ax, ax
    mov es, ax
    mov byte [es:0x7DFE], 0xFF
    
    ; Проверяем
    mov ax, 0xFFFF
    mov es, ax
    cmp byte [es:bx], 0xFF
    
    ; Восстанавливаем значения
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
    
    ; CF=0 если A20 включена, CF=1 если выключена
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

; ===== TAGFS BOOTLOADER DRIVER =====
; Load kernel via TagFS filesystem

load_kernel_tagfs:
    mov si, msg_loading_tagfs
    call print_string_16

    ; Step 1: Read TagFS superblock from sector 1034
    call tagfs_read_superblock
    jc .tagfs_error

    ; Step 2: Find kernel file by tag "type:kernel"
    call tagfs_find_kernel
    jc .kernel_not_found

    ; Step 3: Load kernel file to 0x10000
    call tagfs_load_kernel_file
    jc .load_error

    ; Success
    mov si, msg_kernel_loaded_tagfs
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

; Read TagFS superblock from sector 1034
; Returns: CF=0 on success, CF=1 on error
tagfs_read_superblock:
    push ax
    push dx
    push si

    ; Setup DAP for superblock read
    mov si, dap_tagfs_superblock
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .error

    ; Verify magic number
    mov ax, 0x910
    mov es, ax
    xor bx, bx
    mov eax, [es:bx]        ; Read magic at offset 0
    cmp eax, TAGFS_MAGIC
    jne .error

    ; Restore ES
    xor ax, ax
    mov es, ax

    clc                     ; Success
    pop si
    pop dx
    pop ax
    ret

.error:
    xor ax, ax
    mov es, ax
    stc                     ; Error
    pop si
    pop dx
    pop ax
    ret

; Find kernel file by scanning metadata for tag "type:kernel"
; Returns: CF=0 on success (kernel_file_id, start_block, block_count set)
;          CF=1 on error
tagfs_find_kernel:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; We need to scan metadata sectors starting at 1035
    ; For now, scan first 64 entries (64 sectors)
    mov cx, 64              ; Max metadata entries to scan

.scan_loop:
    push cx                 ; Save loop counter

    ; Calculate metadata sector: 1035 + (64 - CX)
    mov ax, 64
    sub ax, cx
    add ax, TAGFS_METADATA_START
    mov [dap_tagfs_metadata + 8], ax  ; Update LBA sector (low word)

    ; Read metadata sector
    mov si, dap_tagfs_metadata
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13
    jc .scan_next

    ; Check metadata magic at offset 0
    mov ax, 0x930
    mov es, ax
    xor bx, bx
    mov eax, [es:bx]
    cmp eax, TAGFS_METADATA_MAGIC
    jne .scan_next

    ; Check flags at offset 8 (ACTIVE bit)
    mov al, [es:bx + 8]
    test al, TAGFS_FILE_ACTIVE
    jz .scan_next

    ; Check tags (offset 80, 16 tags × 24 bytes)
    mov di, 80              ; Tags array offset

    mov bx, 16              ; 16 tags max per file

.tag_loop:
    ; Tag structure: type(1) + key(11) + value(12) = 24 bytes
    ; We're looking for key="type" value="kernel"

    ; SECURITY: Bounds check to prevent reading beyond sector boundary
    ; Ensure entire tag (24 bytes) fits within 512-byte sector
    mov ax, di
    add ax, 24              ; Check if tag end is within bounds
    cmp ax, 512
    jae .next_tag           ; Skip if tag extends beyond sector

    ; Check key (offset 1 in tag structure)
    lea si, [di + 1]        ; SI = tag.key address

    ; Additional bounds check for key string access (di+1 to di+5)
    mov ax, di
    add ax, 5               ; di+1+4 bytes for "type\0"
    cmp ax, 512
    jae .next_tag           ; Skip if key access would read beyond sector

    mov ax, es
    push ds
    mov ds, ax              ; DS = ES for string compare

    ; Compare "type" (4 bytes + null terminator)
    mov al, [si]
    cmp al, 't'
    jne .next_tag_pop
    mov al, [si+1]
    cmp al, 'y'
    jne .next_tag_pop
    mov al, [si+2]
    cmp al, 'p'
    jne .next_tag_pop
    mov al, [si+3]
    cmp al, 'e'
    jne .next_tag_pop
    mov al, [si+4]
    test al, al
    jnz .next_tag_pop

    ; Check value (offset 12 in tag structure)
    lea si, [di + 12]

    ; Additional bounds check for value string access (di+12 to di+18)
    mov ax, di
    add ax, 19              ; di+12+6 bytes for "kernel\0"
    cmp ax, 512
    jae .next_tag_pop       ; Skip if value access would read beyond sector

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

    ; FOUND! Extract metadata
    pop ds                  ; Restore DS
    xor bx, bx
    mov eax, [es:bx + 4]    ; file_id at offset 4
    mov [kernel_file_id], ax

    mov eax, [es:bx + 20]   ; start_block at offset 20
    mov [kernel_start_block], eax

    mov eax, [es:bx + 24]   ; block_count at offset 24
    mov [kernel_block_count], eax

    ; Read data_start_sector from superblock (offset 32)
    ; Superblock is at 0x9100
    mov ax, 0x910
    push es
    mov es, ax
    xor bx, bx
    mov eax, [es:bx + 32]   ; data_start_sector
    mov [tagfs_data_start], eax
    pop es

    xor ax, ax
    mov es, ax

    pop cx                  ; Clean stack
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc                     ; Success
    ret

.next_tag_pop:
    pop ds

.next_tag:
    add di, 24              ; Next tag
    dec bx
    jnz .tag_loop

.scan_next:
    pop cx                  ; Restore counter
    dec cx
    jnz .scan_loop

    ; Not found
    xor ax, ax
    mov es, ax
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc                     ; Error
    ret

; Load kernel file from TagFS data region
; Uses: kernel_start_block, kernel_block_count, tagfs_data_start
; Returns: CF=0 on success, CF=1 on error
tagfs_load_kernel_file:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Calculate starting sector:
    ; sector = tagfs_data_start + (start_block * 8)
    ; (8 sectors per 4KB block)
    mov eax, [kernel_start_block]
    shl eax, 3              ; Multiply by 8
    add eax, [tagfs_data_start]
    mov [kernel_load_sector], eax

    ; Calculate total sectors to load:
    ; sectors = block_count * 8
    mov eax, [kernel_block_count]
    shl eax, 3
    mov [kernel_load_sectors], ax

    ; Load kernel in chunks of 64 sectors
    mov cx, [kernel_load_sectors]
    mov ebx, [kernel_load_sector]
    mov di, 0x1000          ; Start segment 0x1000:0x0000

.load_loop:
    ; Determine chunk size (min 64, remaining)
    mov ax, 64
    cmp cx, 64
    jae .load_chunk
    mov ax, cx              ; Last chunk < 64 sectors

.load_chunk:
    push cx
    push ax

    ; Setup DAP for this chunk
    mov [dap_kernel_chunk + 2], ax    ; Sector count
    mov [dap_kernel_chunk + 4], word 0x0000  ; Offset
    mov [dap_kernel_chunk + 6], di    ; Segment
    mov [dap_kernel_chunk + 8], ebx   ; LBA

    ; Read chunk
    mov si, dap_kernel_chunk
    mov ah, 0x42
    mov dl, BOOT_DISK
    int 0x13

    pop ax
    pop cx

    jc .load_error

    ; Update for next chunk
    movzx eax, ax
    add ebx, eax            ; Next LBA
    sub cx, ax              ; Remaining sectors

    ; Update segment (AX sectors * 512 bytes / 16 bytes per segment)
    shl ax, 5               ; sectors * 32 = segment increment
    add di, ax

    test cx, cx
    jnz .load_loop

    ; Success
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

; Keep old function for fallback (renamed)
load_kernel_simple_old:
    mov si, msg_loading_kernel
    call print_string_16

    ; Проверка поддержки INT 13h Extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, 0x80
    int 0x13
    jc .disk_error       ; LBA required, no CHS fallback

    ; Используем INT 13h Extensions (LBA)
    ; Загружаем 1024 сектора (512KB) начиная с LBA 10
    ; По 64 сектора за раз (безопасное значение для BIOS compatibility)

    ; Часть 1: 64 сектора
    mov si, dap1
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 2: 64 сектора
    mov si, dap2
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 3: 64 сектора
    mov si, dap3
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 4: 64 сектора
    mov si, dap4
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 5: 64 сектора
    mov si, dap5
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 6: 64 сектора
    mov si, dap6
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 7: 64 сектора
    mov si, dap7
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 8: 64 сектора
    mov si, dap8
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 9: 64 сектора (LBA 522-585)
    mov si, dap9
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error
    ; Части 10-16: дополнительные 448 секторов (64*7 = 448)
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
    
    ; Проверка загрузки (проверяем первые 4 байта)
    mov ax, 0x1000
    mov es, ax
    mov bx, 0x0000
    mov eax, [es:bx]
    test eax, eax
    jz .empty_kernel
    
    ; Восстановка ES
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

    ; Очистим e820 буфер (0x500 = segment 0x50, offset 0)
    mov ax, 0x50
    mov es, ax
    xor di, di
    mov cx, 1024        ; Увеличиваем буфер
    xor ax, ax
    rep stosw

    ; E820 detection
    xor ebx, ebx
    mov edx, 0x534D4150    ; 'SMAP'
    mov ax, 0x50
    mov es, ax
    xor di, di
    xor bp, bp             ; Счетчик записей

.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .e820_fail
    
    ; Проверяем подпись
    cmp eax, 0x534D4150
    jne .e820_fail
    
    ; Проверяем размер записи
    cmp ecx, 20
    jl .skip_entry
    
    ; Увеличиваем счетчик и указатель
    inc bp
    add di, 24
    
.skip_entry:
    ; Check if more entries exist
    test ebx, ebx
    jnz .e820_loop

    ; Восстанавливаем ES перед сохранением
    xor ax, ax
    mov es, ax

    ; Save entry count
    mov [E820_COUNT_ADDR], bp

    ; Calculate total size: bp * 24 bytes per entry
    ; 24 = 16 + 8, so: (bp << 4) + (bp << 3)
    mov ax, bp
    shl ax, 4               ; AX = bp * 16
    mov cx, bp
    shl cx, 3               ; CX = bp * 8
    add ax, cx              ; AX = bp * 24
    mov [E820_SIZE_ADDR], ax

    mov si, msg_e820_success
    call print_string_16
    ret

.e820_fail:
    mov si, msg_e820_fail
    call print_string_16

    ; Fallback: create minimal memory map at 0x500
    mov ax, 0x50
    mov es, ax
    xor di, di

    ; Entry 1: 0-640KB (usable RAM)
    mov dword [es:di], 0x00000000      ; Base address low
    mov dword [es:di+4], 0x00000000    ; Base address high
    mov dword [es:di+8], 0x0009FC00    ; Length: 640KB
    mov dword [es:di+12], 0x00000000   ; Length high
    mov dword [es:di+16], 1            ; Type: usable
    mov dword [es:di+20], 0            ; Extended attributes
    add di, 24

    ; Entry 2: 1MB+ (detect size via INT 15h AH=88h)
    mov ah, 0x88
    int 0x15
    jc .memory_fail

    mov dword [es:di], 0x00100000      ; Base: 1MB
    mov dword [es:di+4], 0x00000000
    movzx eax, ax
    shl eax, 10                        ; Convert KB to bytes
    mov [es:di+8], eax
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1            ; Type: usable
    mov dword [es:di+20], 0

    ; Восстанавливаем ES
    xor ax, ax
    mov es, ax

    ; Save entry count and size (2 entries * 24 bytes = 48)
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

; Validate page table location is in usable RAM via E820 map
; Checks if PAGE_TABLE_BASE (0x820000-0x823FFF) falls within usable memory
; Halts system with error if page tables would be in reserved/unusable region
validate_page_table_location:
    push ax
    push bx
    push cx
    push si
    push di
    push es

    ; Get E820 entry count
    mov cx, [E820_COUNT_ADDR]
    test cx, cx
    jz .validation_failed       ; No E820 map = unsafe

    ; Setup ES:DI to point to E820 map
    mov ax, 0x50
    mov es, ax
    xor di, di

    ; Constants for validation
    ; PAGE_TABLE_BASE = 0x820000 (8MB + 128KB)
    ; PAGE_TABLE_SIZE = 0x4000 (16KB)
    ; Need to check if [0x820000, 0x824000) is in usable RAM

.check_loop:
    ; Read base address (64-bit at offset 0)
    mov eax, [es:di]            ; Base address low
    mov ebx, [es:di+4]          ; Base address high

    ; Read length (64-bit at offset 8)
    mov esi, [es:di+8]          ; Length low
    mov edx, [es:di+12]         ; Length high

    ; Calculate end address: end = base + length
    add esi, eax                ; End low = base_low + length_low
    adc edx, ebx                ; End high = base_high + length_high (with carry)

    ; Check memory type (offset 16) - must be type 1 (usable)
    mov eax, [es:di+16]         ; Type is 32-bit dword
    cmp eax, 1
    jne .next_entry             ; Skip non-usable entries

    ; Reload base address (was overwritten)
    mov eax, [es:di]            ; Base address low
    mov ebx, [es:di+4]          ; Base address high

    ; Now check if PAGE_TABLE_BASE falls within [base, end)
    ; PAGE_TABLE_BASE = 0x820000 = high:0x00, low:0x820000

    ; Check if base > 0x820000 (if so, range doesn't cover page tables)
    ; Base is in EBX:EAX, compare with 0x00:0x820000
    cmp ebx, 0x00
    ja .next_entry              ; Base high > 0, too high
    jb .check_end               ; Base high < 0, impossible but check end anyway
    ; Base high == 0, compare low parts
    cmp eax, 0x820000
    ja .next_entry              ; Base low > 0x820000, too high

.check_end:
    ; Check if end <= 0x820000 (if so, range doesn't cover page tables)
    ; End is in EDX:ESI, need end > 0x824000 (0x00:0x824000)
    cmp edx, 0x00
    ja .found                   ; End high > 0, definitely covers our range
    jb .next_entry              ; End high < 0, impossible
    ; End high == 0, compare low parts
    cmp esi, 0x824000           ; Need end >= 0x824000
    jae .found                  ; Found a region that covers page tables

.next_entry:
    add di, 24                  ; Move to next E820 entry (24 bytes each)
    dec cx
    jnz .check_loop

    ; No suitable region found
    jmp .validation_failed

.found:
    ; FOUND! Page tables are in usable RAM region
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
    ; Page table location is NOT in usable RAM or insufficient memory
    xor ax, ax
    mov es, ax

    mov si, msg_page_table_unsafe
    call print_string_16
    call wait_key
    cli
    hlt
    jmp $

[BITS 32]
; ===== ФУНКЦИИ 32-BIT РЕЖИМА =====

; Calculate required identity mapping size based on E820 memory map
; Returns: ECX = number of 2MB pages to map (min 64, max 512)
calculate_identity_map_size:
    push eax
    push ebx
    push edx
    push esi
    push edi

    ; Read E820 entry count
    movzx ecx, word [E820_COUNT_ADDR]
    test ecx, ecx
    jz .use_default

    ; Initialize max address to 0
    xor edx, edx        ; EDX = max_address_high
    xor ebx, ebx        ; EBX = max_address_low

    ; Loop through E820 entries
    mov esi, E820_MAP_ADDR

.e820_loop:
    ; Read entry type (offset 16)
    mov eax, [esi + 16]
    cmp eax, 1              ; Type 1 = usable RAM
    jne .skip_entry

    ; Calculate end address: base + length
    mov eax, [esi]          ; Base address low
    mov edi, [esi + 4]      ; Base address high
    add eax, [esi + 8]      ; Add length low
    adc edi, [esi + 12]     ; Add length high (with carry)

    ; Compare with current max (64-bit compare)
    cmp edi, edx
    ja .update_max
    jb .skip_entry
    cmp eax, ebx
    jbe .skip_entry

.update_max:
    mov ebx, eax
    mov edx, edi

.skip_entry:
    add esi, 24             ; Next entry (24 bytes)
    loop .e820_loop

    ; Convert max address to 2MB page count
    ; max_address is in EDX:EBX
    ; Divide by 2MB (0x200000)
    ; We only need EDX and high bits of EBX
    mov eax, edx
    shl eax, 11             ; EDX * 2048 (shift left 11 = multiply by 2048)
    shr ebx, 21             ; EBX / 2MB (shift right 21)
    add eax, ebx            ; Total 2MB pages
    inc eax                 ; Round up

    ; Apply safety limits
    cmp eax, 64
    jae .check_max
    mov eax, 64             ; Minimum 64 pages (128MB)
    jmp .done

.check_max:
    cmp eax, 512
    jbe .done
    mov eax, 512            ; Maximum 512 pages (1GB)
    jmp .done

.use_default:
    mov eax, 64             ; Default: 128MB

.done:
    mov ecx, eax            ; Return value in ECX

    pop edi
    pop esi
    pop edx
    pop ebx
    pop eax
    ret

setup_paging_fixed:
    ; Calculate required identity map size dynamically
    call calculate_identity_map_size
    ; ECX now contains number of 2MB pages to map

    push ecx              ; Save page count for later

    ; Очистка области для таблиц страниц (16KB)
    mov edi, PAGE_TABLE_BASE
    mov ecx, 4096         ; 16KB / 4 = 4096 dwords
    xor eax, eax
    rep stosd

    ; PML4 Table (0x820000) - только первая запись (64-bit entry)
    mov dword [PAGE_TABLE_BASE], PAGE_TABLE_BASE + 0x1000 + 3  ; PDPT at +4KB
    mov dword [PAGE_TABLE_BASE + 4], 0x00000000

    ; PDPT (0x821000) - только первая запись (64-bit entry)
    mov dword [PAGE_TABLE_BASE + 0x1000], PAGE_TABLE_BASE + 0x2000 + 3  ; PD at +8KB
    mov dword [PAGE_TABLE_BASE + 0x1004], 0x00000000

    ; PD (0x822000) - dynamic mapping based on E820
    mov edi, PAGE_TABLE_BASE
    add edi, 0x2000       ; PD offset
    mov eax, 0x000083     ; Present, Writable, Page Size (2MB)
    pop ecx               ; Restore page count

.fill_pd:
    mov [edi], eax        ; Lower 32 bits
    mov dword [edi+4], 0  ; Upper 32 bits (explicit zero)
    add eax, 0x200000     ; Следующие 2MB
    add edi, 8
    loop .fill_pd

    ret

enable_long_mode_fixed:
    ; Включение PAE в CR4
    mov eax, cr4
    or eax, (1 << 5)      ; PAE bit
    mov cr4, eax

    ; Загрузка PML4 в CR3 (now at 0x500000)
    mov eax, PAGE_TABLE_BASE
    mov cr3, eax
    
    ; Включение Long Mode и NX в EFER
    mov ecx, 0xC0000080   ; EFER MSR
    rdmsr
    or eax, (1 << 8)      ; LME bit (Long Mode Enable)
    or eax, (1 << 11)     ; NXE bit (No-Execute Enable) - CRITICAL FIX
    wrmsr
    
    ; Включение paging в CR0
    mov eax, cr0
    or eax, (1 << 31)     ; PG bit
    mov cr0, eax
    
    ret

; ===== ИСПРАВЛЕННЫЙ GDT =====
align 8
gdt_start:
    ; 0x00: Null Descriptor
    dq 0x0000000000000000

    ; 0x08: 32-bit Kernel Code Segment
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x9A         ; Access: Present, Ring 0, Code, Executable, Readable
    db 0xCF         ; Flags: 4KB granularity, 32-bit, Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

    ; 0x10: 32-bit Kernel Data Segment
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x92         ; Access: Present, Ring 0, Data, Writable
    db 0xCF         ; Flags: 4KB granularity, 32-bit, Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

    ; 0x18: 64-bit Kernel Code Segment
    dw 0x0000       ; Limit (ignored in 64-bit)
    dw 0x0000       ; Base (ignored in 64-bit)
    db 0x00         ; Base (ignored in 64-bit)
    db 0x9A         ; Access: Present, Ring 0, Code, Executable, Readable
    db 0x20         ; Flags: Long mode bit (L=1), все остальные 0
    db 0x00         ; Base (ignored in 64-bit)

    ; 0x20: 64-bit Kernel Data Segment
    dw 0x0000       ; Limit (ignored in 64-bit)
    dw 0x0000       ; Base (ignored in 64-bit)
    db 0x00         ; Base (ignored in 64-bit)
    db 0x92         ; Access: Present, Ring 0, Data, Writable
    db 0x00         ; Flags: (все биты 0 для data segment)
    db 0x00         ; Base (ignored in 64-bit)

gdt_end:

align 4
gdt_descriptor:
    dw gdt_end - gdt_start - 1    ; Limit
    dd gdt_start                  ; Base address (32-bit в 16-bit режиме)

; ===== DAP STRUCTURES FOR INT 13h EXTENSIONS (LBA MODE) =====
; 16 chunks of 64 sectors each = 1024 sectors = 512KB total
; Each DAP loads 32KB (64 sectors * 512 bytes)
align 4
dap1:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x1000           ; Segment (0x1000:0x0000 = 0x10000 physical)
    dq 17               ; Starting LBA sector: 17

align 4
dap2:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x1800           ; Segment (0x1800:0x0000 = 0x18000 physical)
    dq 81               ; Starting LBA sector: 81 (17 + 64)

align 4
dap3:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x2000           ; Segment (0x2000:0x0000 = 0x20000 physical)
    dq 145              ; Starting LBA sector: 145 (17 + 64 + 64)

align 4
dap4:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x2800           ; Segment (0x2800:0x0000 = 0x28000 physical)
    dq 209              ; Starting LBA sector: 209 (17 + 64*3)

align 4
dap5:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x3000           ; Segment (0x3000:0x0000 = 0x30000 physical)
    dq 273              ; Starting LBA sector: 273 (17 + 64*4)

align 4
dap6:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x3800           ; Segment (0x3800:0x0000 = 0x38000 physical)
    dq 337              ; Starting LBA sector: 337 (17 + 64*5)

align 4
dap7:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x4000           ; Segment (0x4000:0x0000 = 0x40000 physical)
    dq 401              ; Starting LBA sector: 401 (17 + 64*6)

align 4
dap8:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x4800           ; Segment (0x4800:0x0000 = 0x48000 physical)
    dq 465              ; Starting LBA sector: 465 (17 + 64*7)

align 4
dap9:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64 (loads additional 32KB)
    dw 0x0000           ; Offset
    dw 0x5000           ; Segment (0x5000:0x0000 = 0x50000 physical)
    dq 529              ; Starting LBA sector: 529 (17 + 64*8)

align 4
dap10:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x5800           ; Segment (0x5800:0x0000 = 0x58000 physical)
    dq 593              ; Starting LBA sector: 593 (17 + 64*9)

align 4
dap11:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x6000           ; Segment (0x6000:0x0000 = 0x60000 physical)
    dq 657              ; Starting LBA sector: 657 (17 + 64*10)

align 4
dap12:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x6800           ; Segment (0x6800:0x0000 = 0x68000 physical)
    dq 721              ; Starting LBA sector: 721 (17 + 64*11)

align 4
dap13:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x7000           ; Segment (0x7000:0x0000 = 0x70000 physical)
    dq 785              ; Starting LBA sector: 785 (17 + 64*12)

align 4
dap14:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x7800           ; Segment (0x7800:0x0000 = 0x78000 physical)
    dq 849              ; Starting LBA sector: 849 (17 + 64*13)

align 4
dap15:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x8000           ; Segment (0x8000:0x0000 = 0x80000 physical)
    dq 913              ; Starting LBA sector: 913 (17 + 64*14)

align 4
dap16:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 64               ; Sector count: 64
    dw 0x0000           ; Offset
    dw 0x8800           ; Segment (0x8800:0x0000 = 0x88000 physical)
    dq 977              ; Starting LBA sector: 977 (17 + 64*15)

; ===== TAGFS DAP STRUCTURES =====
align 4
dap_tagfs_superblock:
    db 0x10                         ; DAP size
    db 0                            ; Reserved
    dw 1                            ; Sector count: 1
    dw 0x0000                       ; Offset
    dw 0x0910                       ; Segment (0x910:0x0000 = 0x9100)
    dq TAGFS_SUPERBLOCK_SECTOR      ; LBA: 1034

align 4
dap_tagfs_metadata:
    db 0x10                         ; DAP size
    db 0                            ; Reserved
    dw 1                            ; Sector count: 1
    dw 0x0000                       ; Offset
    dw 0x0930                       ; Segment (0x930:0x0000 = 0x9300)
    dq TAGFS_METADATA_START         ; LBA: 1035 (updated dynamically)

align 4
dap_kernel_chunk:
    db 0x10                         ; DAP size
    db 0                            ; Reserved
    dw 64                           ; Sector count (updated dynamically)
    dw 0x0000                       ; Offset (updated dynamically)
    dw 0x1000                       ; Segment (updated dynamically)
    dq 0                            ; LBA (updated dynamically)

; ===== TAGFS VARIABLES =====
align 4
kernel_file_id:         dw 0
kernel_start_block:     dd 0
kernel_block_count:     dd 0
tagfs_data_start:       dd 0
kernel_load_sector:     dd 0
kernel_load_sectors:    dw 0

; ===== MESSAGES =====
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


; Заполнение до 8KB
times 8192-($-$$) db 0