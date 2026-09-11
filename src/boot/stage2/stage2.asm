[BITS 16]
[ORG 0x8000]
DEFAULT ABS


KERNEL_BOUNCE_ADDR    equ 0x10000
KERNEL_BOUNCE_SEG     equ 0x1000
KERNEL_RUN_ADDR       equ 0x100000
KERNEL_MAX_SIZE       equ 0x2000000

PAGE_TABLE_SIZE       equ 0x8000
GUARD_PAGE_SIZE       equ 0x200000
BOOT_STACK_SIZE       equ 0x10000

E820_COUNT_ADDR       equ 0x500
E820_SIZE_ADDR        equ 0x502
E820_MAP_ADDR         equ 0x504
E820_MAX_ENTRIES      equ 128
E820_SEG              equ E820_COUNT_ADDR >> 4
E820_REGION_WORDS     equ (4 + E820_MAX_ENTRIES * 24 + 1) / 2

STAGE2_SIGNATURE      equ 0x2907

%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must come from the build (-DSTAGE2_SECTORS=...)"
%endif
%ifndef BOARDING_PASS_ADDR
  %error "BOARDING_PASS_ADDR must come from the build (-DBOARDING_PASS_ADDR=...)"
%endif
%ifndef BOOT_INFO_ADDR
  %error "BOOT_INFO_ADDR must come from the build (-DBOOT_INFO_ADDR=...)"
%endif

BOOT_INFO_MAGIC       equ 0x42583031
BOOT_INFO_VERSION     equ 1

BOARDING_PASS_MAGIC   equ 0x53415042
BOARDING_PASS_VERSION equ 1
BOARDING_PASS_BYTES   equ 512
BOARDING_HDR_BYTES    equ 16
BOARDING_STAMP_VOLUME equ 1
BOARDING_STAMP_MEDIUM equ 2
BOARDING_STAMP_LOADER equ 3
BOARDING_STAMP_SEAL   equ 4
BOARDING_FIRMWARE_BIOS equ 0
DEED_OFF_MAGIC          equ 0
DEED_OFF_PROLOGUE_BYTES equ 8
DEED_OFF_STAMP_BYTES    equ 10
DEED_OFF_CRC32          equ 12
DEED_OFF_UUID           equ 16
DEED_OFF_SECTORS        equ 32
DEED_OFF_TAIL_SECTOR    equ 40
DEED_OFF_ROLE           equ 48
DEED_PROLOGUE_BYTES     equ 52

DEED_MAGIC_LO           equ 0x44584F42
DEED_MAGIC_HI           equ 0x00444545
DEED_ROLE_HEAD          equ 1
DEED_ROLE_TAIL          equ 2
DEED_SECTORS            equ 8
DEED_BYTES              equ DEED_SECTORS * 512

VOLUME_STAMP_BOOT       equ 4
BOOT_KERNEL_BLOCK       equ 0
BOOT_KERNEL_BLOCKS      equ 4
BOOT_KERNEL_BYTES       equ 8

MBR_TABLE_OFFSET        equ 446
MBR_ENTRY_BYTES         equ 16
MBR_ENTRY_COUNT         equ 4
MBR_ENTRY_TYPE          equ 4
MBR_ENTRY_START_LBA     equ 8
MBR_ENTRY_SECTORS       equ 12
MBR_TYPE_BOXOS          equ 0x7F
MBR_TYPE_PROTECTIVE     equ 0xEE

MBR_ADDR                equ 0xA200
MBR_SEG                 equ MBR_ADDR >> 4

GPT_HEADER_LBA          equ 1
GPT_SIG_LO              equ 0x20494645
GPT_SIG_HI              equ 0x54524150
GPT_OFF_HEADER_BYTES    equ 0x0C
GPT_OFF_HEADER_CRC      equ 0x10
GPT_OFF_ENTRY_LBA       equ 0x48
GPT_OFF_ENTRY_COUNT     equ 0x50
GPT_OFF_ENTRY_BYTES     equ 0x54
GPT_OFF_ENTRY_CRC       equ 0x58
GPT_HEADER_MIN_BYTES    equ 92
GPT_ENTRY_MIN_BYTES     equ 128
GPT_ARRAY_MAX_BYTES     equ 65536

GPT_ENTRY_OFF_TYPE      equ 0
GPT_ENTRY_OFF_FIRST     equ 0x20
GPT_ENTRY_OFF_LAST      equ 0x28

GPT_SCRATCH_ADDR        equ 0xC000
GPT_SCRATCH_SEG         equ GPT_SCRATCH_ADDR >> 4
GPT_SCRATCH_BYTES       equ 4096
GPT_SCRATCH_SECTORS     equ GPT_SCRATCH_BYTES / 512
DEED_ADDR               equ 0xB000
DEED_SEG                equ DEED_ADDR >> 4

KERNEL_HDR_MAGIC        equ 0x4E52454B
KERNEL_HDR_MAGIC_HI     equ 0x4C45
KERNEL_HDR_VERSION      equ 1

TAGFS_BLOCK_SIZE              equ 4096
TAGFS_SECTOR_SIZE             equ 512
TAGFS_SECTORS_PER_BLOCK       equ TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE
TAGFS_SECTORS_PER_BLOCK_LOG2  equ 3
KERNEL_MAX_BLOCKS             equ KERNEL_MAX_SIZE / TAGFS_BLOCK_SIZE

jmp short past_sig
dw STAGE2_SIGNATURE
past_sig:

start_stage2:
    cli
    cld

    mov [boot_drive_saved], dl

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ss, ax
    mov sp, 0x7000

    mov ax, 0x0003
    int 0x10

    sti

    mov si, msg_stage2_start
    call print_string_16

    call enable_a20_enhanced
    call enter_unreal_mode
    call detect_memory_e820
    call load_kernel_tagfs
    call validate_kernel_magic
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

    mov esp, 0x7C00

    push dword 0
    popf

    mov edi, 0xB8000
    mov al, 'P'
    mov ah, 0x0F
    mov [edi], ax
    mov al, 'M'
    mov [edi+2], ax

    call compute_dynamic_layout

    call validate_dynamic_layout

    mov esp, [dynamic_stack_base]

    call setup_paging
    call enable_long_mode

    jmp 0x18:long_mode_start

compute_dynamic_layout:
    push eax

    mov eax, [KERNEL_RUN_ADDR + 12]

    add eax, 0x1FFFFF
    and eax, 0xFFE00000
    mov [guard1_base], eax

    add eax, GUARD_PAGE_SIZE
    mov [dynamic_pt_base], eax

    add eax, PAGE_TABLE_SIZE
    add eax, 0x1FFFFF
    and eax, 0xFFE00000
    mov [guard2_base], eax

    add eax, GUARD_PAGE_SIZE
    add eax, BOOT_STACK_SIZE
    mov [dynamic_stack_base], eax

    pop eax
    ret

validate_dynamic_layout:
    push eax
    push ebx
    push ecx
    push edx
    push esi

    mov edx, [dynamic_stack_base]

    movzx ecx, word [E820_COUNT_ADDR]
    test ecx, ecx
    jz .vdl_fail

    mov esi, E820_MAP_ADDR

.vdl_loop:
    cmp dword [esi+16], 1
    jne .vdl_next
    cmp dword [esi+4], 0
    jne .vdl_next

    mov eax, [esi]
    cmp eax, [dynamic_pt_base]
    ja .vdl_next

    mov ebx, eax
    add ebx, [esi+8]
    jc .vdl_found
    cmp ebx, edx
    jae .vdl_found

.vdl_next:
    add esi, 24
    dec ecx
    jnz .vdl_loop

.vdl_fail:
    mov edi, 0xB8000 + 160
    mov ah, 0x4F
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

    mov esp, [dynamic_stack_base]

    push qword 0
    popf

    mov rdi, 0xB8000
    mov al, 'L'
    mov ah, 0x0F
    mov [rdi+4], ax
    mov al, 'M'
    mov [rdi+6], ax

    mov rax, [KERNEL_RUN_ADDR]
    test rax, rax
    jz .kernel_not_loaded

    mov dword [BOOT_INFO_ADDR],      BOOT_INFO_MAGIC
    mov dword [BOOT_INFO_ADDR+4],    BOOT_INFO_VERSION
    mov dword [BOOT_INFO_ADDR+8],    E820_MAP_ADDR
    mov ax, [E820_COUNT_ADDR]
    mov word  [BOOT_INFO_ADDR+12],   ax
    mov word  [BOOT_INFO_ADDR+14],   0
    mov dword [BOOT_INFO_ADDR+16],   KERNEL_RUN_ADDR

    mov eax, [KERNEL_RUN_ADDR + 12]
    add eax, 0xFFF
    and eax, 0xFFFFF000
    mov dword [BOOT_INFO_ADDR+20],   eax

    movzx eax, byte [boot_drive_saved]
    mov byte  [BOOT_INFO_ADDR+24],   al
    mov byte  [BOOT_INFO_ADDR+25],   0
    mov word  [BOOT_INFO_ADDR+26],   0

    mov eax, [dynamic_pt_base]
    mov dword [BOOT_INFO_ADDR+28],   eax
    mov eax, [dynamic_stack_base]
    mov dword [BOOT_INFO_ADDR+32],   eax
    mov dword [BOOT_INFO_ADDR+36],   40

    call write_boarding_pass

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

write_boarding_pass:
    mov dword [BOARDING_PASS_ADDR],     BOARDING_PASS_MAGIC
    mov word  [BOARDING_PASS_ADDR+4],   BOARDING_PASS_VERSION
    mov word  [BOARDING_PASS_ADDR+6],   BOARDING_HDR_BYTES
    mov word  [BOARDING_PASS_ADDR+8],   72
    mov word  [BOARDING_PASS_ADDR+10],  BOARDING_PASS_BYTES
    mov word  [BOARDING_PASS_ADDR+12],  4
    mov word  [BOARDING_PASS_ADDR+14],  0

    mov word  [BOARDING_PASS_ADDR+16],  BOARDING_STAMP_VOLUME
    mov word  [BOARDING_PASS_ADDR+18],  16
    mov rax, [DEED_ADDR + DEED_OFF_UUID]
    mov [BOARDING_PASS_ADDR+20], rax
    mov rax, [DEED_ADDR + DEED_OFF_UUID + 8]
    mov [BOARDING_PASS_ADDR+28], rax

    mov word  [BOARDING_PASS_ADDR+36],  BOARDING_STAMP_MEDIUM
    mov word  [BOARDING_PASS_ADDR+38],  4
    mov byte  [BOARDING_PASS_ADDR+40],  BOARDING_FIRMWARE_BIOS
    mov al, [boot_drive_saved]
    mov byte  [BOARDING_PASS_ADDR+41],  al
    mov word  [BOARDING_PASS_ADDR+42],  0

    mov word  [BOARDING_PASS_ADDR+44],  BOARDING_STAMP_LOADER
    mov word  [BOARDING_PASS_ADDR+46],  16
    mov rax, [boarding_loader_name]
    mov [BOARDING_PASS_ADDR+48], rax
    mov eax, [boarding_loader_name+8]
    mov [BOARDING_PASS_ADDR+56], eax
    mov word  [BOARDING_PASS_ADDR+60],  1
    mov word  [BOARDING_PASS_ADDR+62],  0

    mov word  [BOARDING_PASS_ADDR+64],  BOARDING_STAMP_SEAL
    mov word  [BOARDING_PASS_ADDR+66],  4

    mov esi, BOARDING_PASS_ADDR
    mov ecx, 68
    mov eax, 0xFFFFFFFF
.seal_byte:
    movzx edx, byte [rsi]
    xor eax, edx
    mov edx, 8
.seal_bit:
    shr eax, 1
    jnc .seal_next
    xor eax, 0xEDB88320
.seal_next:
    dec edx
    jnz .seal_bit
    inc rsi
    dec ecx
    jnz .seal_byte
    not eax
    mov [BOARDING_PASS_ADDR+68], eax
    ret


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
    test edx, (1 << 29)
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

    mov ax, 0x2401
    int 0x15
    call test_a20
    jnc .a20_done

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

    in al, 0x92
    test al, 2
    jnz .a20_done
    or al, 2
    and al, 0xFE
    out 0x92, al

.a20_done:
    pop cx
    pop ax

    call test_a20
    jc .a20_fatal

    mov si, msg_a20_enabled
    call print_string_16
    ret

.a20_fatal:
    mov si, msg_a20_fatal
    call print_string_16
    cli
    hlt

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
    mov ax, 0
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

enter_unreal_mode:
    cli

    lgdt [unreal_gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    jmp 0x0000:.unreal_flush
.unreal_flush:
    xor ax, ax
    mov ds, ax
    mov es, ax

    sti

    mov si, msg_unreal_mode
    call print_string_16
    ret

restore_unreal_mode:
    cli

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    jmp 0x0000:.rum_flush
.rum_flush:
    xor ax, ax
    mov ds, ax
    mov es, ax

    sti
    ret

load_kernel_tagfs:
    mov si, msg_loading_tagfs
    call print_string_16

    call find_ground
    jnc .got_ground

    cmp byte [boot_drive_saved], 0x80
    je .ground_error
    mov byte [boot_drive_saved], 0x80
    call find_ground
    jc  .ground_error

.got_ground:
    call read_deed
    jc .deed_error

    call find_kernel_in_deed
    jc .no_kernel

    call tagfs_load_kernel_file
    jc .load_error

    mov si, msg_kernel_loaded_tagfs
    call print_string_16
    ret

.ground_error:
    mov si, msg_no_ground
    call print_string_16
    call print_disk_status
    jmp .halt

.deed_error:
    mov si, msg_no_deed
    call print_string_16
    call print_disk_status
    jmp .halt

.no_kernel:
    mov si, msg_kernel_not_found
    call print_string_16
    jmp .halt

.load_error:
    jmp .halt

.halt:
    call wait_key
    cli
    hlt
    jmp $

find_ground:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    mov si, dap_sector0
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc .fail

    mov ax, MBR_SEG
    mov es, ax
    cmp word [es:510], 0xAA55
    jne .no_table

    mov bx, MBR_TABLE_OFFSET
    mov cx, MBR_ENTRY_COUNT
.scan_protective:
    cmp byte [es:bx + MBR_ENTRY_TYPE], MBR_TYPE_PROTECTIVE
    je .is_gpt
    add bx, MBR_ENTRY_BYTES
    loop .scan_protective

    mov bx, MBR_TABLE_OFFSET
    mov cx, MBR_ENTRY_COUNT
.scan_ours:
    cmp byte [es:bx + MBR_ENTRY_TYPE], MBR_TYPE_BOXOS
    je .found
    add bx, MBR_ENTRY_BYTES
    loop .scan_ours

    mov si, msg_no_boxos_partition
    call print_string_16
    jmp .fail_quiet

.found:
    mov eax, [es:bx + MBR_ENTRY_START_LBA]
    mov [ground_start_lba], eax
    mov dword [ground_start_lba + 4], 0
    mov eax, [es:bx + MBR_ENTRY_SECTORS]
    mov [ground_sectors], eax
    cmp dword [ground_sectors], DEED_SECTORS
    jb .too_small

    xor ax, ax
    mov es, ax
    clc
    jmp .out

.is_gpt:
    xor ax, ax
    mov es, ax
    call find_ground_gpt
    jc .fail_quiet
    jmp .out

.no_table:
    mov si, msg_no_table
    call print_string_16
    jmp .fail_quiet

.too_small:
    mov si, msg_ground_too_small
    call print_string_16
    jmp .fail_quiet

.fail:
.fail_quiet:
    xor ax, ax
    mov es, ax
    stc
.out:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

find_ground_gpt:
    push eax
    push ebx
    push ecx
    push edx
    push si
    push di
    push es

    mov dword [gpt_found], 0

    mov dword [dap_gpt + 8], GPT_HEADER_LBA
    mov dword [dap_gpt + 12], 0
    mov word  [dap_gpt + 2], 1
    mov si, dap_gpt
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc .no_header

    mov ax, GPT_SCRATCH_SEG
    mov es, ax
    cmp dword [es:0], GPT_SIG_LO
    jne .not_gpt
    cmp dword [es:4], GPT_SIG_HI
    jne .not_gpt

    mov eax, [es:GPT_OFF_HEADER_BYTES]
    cmp eax, GPT_HEADER_MIN_BYTES
    jb .bad_header
    cmp eax, 512
    ja .bad_header
    mov [gpt_header_bytes], eax

    mov eax, [es:GPT_OFF_HEADER_CRC]
    mov [gpt_want_crc], eax
    mov eax, [es:GPT_OFF_ENTRY_LBA]
    mov [gpt_entry_lba], eax
    mov eax, [es:GPT_OFF_ENTRY_LBA + 4]
    mov [gpt_entry_lba + 4], eax
    mov eax, [es:GPT_OFF_ENTRY_COUNT]
    mov [gpt_entry_count], eax
    mov eax, [es:GPT_OFF_ENTRY_BYTES]
    mov [gpt_entry_bytes], eax
    mov eax, [es:GPT_OFF_ENTRY_CRC]
    mov [gpt_array_crc], eax

    mov edx, 0xFFFFFFFF
    mov ecx, [gpt_header_bytes]
    mov bx, GPT_OFF_HEADER_CRC
    call crc32_chunk_hole
    not edx
    cmp edx, [gpt_want_crc]
    jne .bad_header_crc

    mov eax, [gpt_entry_bytes]
    cmp eax, GPT_ENTRY_MIN_BYTES
    jb .bad_array
    cmp eax, GPT_ARRAY_MAX_BYTES
    ja .bad_array
    mov ecx, [gpt_entry_count]
    test ecx, ecx
    jz .bad_array
    mul ecx
    test edx, edx
    jnz .bad_array
    cmp eax, GPT_ARRAY_MAX_BYTES
    ja .bad_array
    mov [gpt_array_bytes], eax

    mov eax, GPT_SCRATCH_BYTES
    xor edx, edx
    div dword [gpt_entry_bytes]
    test edx, edx
    jnz .bad_array

    mov eax, [gpt_entry_lba]
    mov [dap_gpt + 8], eax
    mov eax, [gpt_entry_lba + 4]
    mov [dap_gpt + 12], eax

    mov ecx, [gpt_array_bytes]
    mov edx, 0xFFFFFFFF

.window:
    test ecx, ecx
    jz .array_done

    mov word [dap_gpt + 2], GPT_SCRATCH_SECTORS
    push ecx
    push edx
    mov si, dap_gpt
    mov dl, [boot_drive_saved]
    call disk_read_dap
    pop edx
    pop ecx
    jc .no_array

    mov ax, GPT_SCRATCH_SEG
    mov es, ax

    mov eax, ecx
    cmp eax, GPT_SCRATCH_BYTES
    jbe .have_len
    mov eax, GPT_SCRATCH_BYTES
.have_len:
    mov [gpt_window_bytes], eax

    push ecx
    mov ecx, eax
    call crc32_chunk
    pop ecx

    call gpt_scan_window

    sub ecx, [gpt_window_bytes]

    push ecx
    mov eax, [gpt_window_bytes]
    add eax, 511
    shr eax, 9
    add [dap_gpt + 8], eax
    adc dword [dap_gpt + 12], 0
    pop ecx
    jmp .window

.array_done:
    not edx
    cmp edx, [gpt_array_crc]
    jne .bad_array_crc

    cmp dword [gpt_found], 0
    je .none_of_ours

    mov eax, [gpt_cand_first]
    mov [ground_start_lba], eax
    mov eax, [gpt_cand_first + 4]
    mov [ground_start_lba + 4], eax

    mov eax, [gpt_cand_last]
    mov edx, [gpt_cand_last + 4]
    sub eax, [gpt_cand_first]
    sbb edx, [gpt_cand_first + 4]
    add eax, 1
    adc edx, 0
    test edx, edx
    jnz .too_long
    mov [ground_sectors], eax
    cmp eax, DEED_SECTORS
    jb .gpt_too_small

    xor ax, ax
    mov es, ax
    clc
    jmp .out

.no_header:
    mov si, msg_gpt_no_header
    call print_string_16
    jmp .fail
.not_gpt:
    mov si, msg_gpt_absent
    call print_string_16
    jmp .fail
.bad_header:
    mov si, msg_gpt_header_len
    call print_string_16
    jmp .fail
.bad_header_crc:
    mov si, msg_gpt_header_crc
    call print_string_16
    jmp .fail
.bad_array:
    mov si, msg_gpt_array_shape
    call print_string_16
    jmp .fail
.no_array:
    mov si, msg_gpt_no_array
    call print_string_16
    jmp .fail
.bad_array_crc:
    mov si, msg_gpt_array_crc
    call print_string_16
    jmp .fail
.none_of_ours:
    mov si, msg_gpt_not_ours
    call print_string_16
    jmp .fail
.too_long:
    mov si, msg_gpt_too_long
    call print_string_16
    jmp .fail
.gpt_too_small:
    mov si, msg_ground_too_small
    call print_string_16
.fail:
    xor ax, ax
    mov es, ax
    stc
.out:
    pop es
    pop di
    pop si
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

gpt_scan_window:
    push eax
    push ebx
    push ecx
    push edx
    push si
    push di

    xor si, si
    mov ecx, [gpt_window_bytes]

.entry:
    cmp ecx, [gpt_entry_bytes]
    jb .done

    cmp dword [gpt_found], 0
    jne .step

    mov di, si
    add di, GPT_ENTRY_OFF_TYPE
    mov eax, [es:di]
    cmp eax, BOXOS_TYPE_GUID_0
    jne .step
    mov eax, [es:di + 4]
    cmp eax, BOXOS_TYPE_GUID_1
    jne .step
    mov eax, [es:di + 8]
    cmp eax, BOXOS_TYPE_GUID_2
    jne .step
    mov eax, [es:di + 12]
    cmp eax, BOXOS_TYPE_GUID_3
    jne .step

    mov di, si
    add di, GPT_ENTRY_OFF_FIRST
    mov eax, [es:di]
    mov [gpt_cand_first], eax
    mov eax, [es:di + 4]
    mov [gpt_cand_first + 4], eax
    mov di, si
    add di, GPT_ENTRY_OFF_LAST
    mov eax, [es:di]
    mov [gpt_cand_last], eax
    mov eax, [es:di + 4]
    mov [gpt_cand_last + 4], eax
    mov dword [gpt_found], 1

.step:
    mov eax, [gpt_entry_bytes]
    add si, ax
    sub ecx, eax
    jmp .entry

.done:
    pop di
    pop si
    pop edx
    pop ecx
    pop eax
    pop ebx
    ret

crc32_chunk:
    mov bx, 0xFFFF
crc32_chunk_hole:
    push eax
    push ecx
    push si
    push di

    xor di, di
.byte_loop:
    test ecx, ecx
    jz .done

    xor eax, eax
    cmp di, bx
    jb .take
    push bx
    add bx, 4
    cmp di, bx
    pop bx
    jb .have
.take:
    mov al, [es:di]
.have:
    xor edx, eax
    mov si, 8
.bit_loop:
    test edx, 1
    jz .no_poly
    shr edx, 1
    xor edx, 0xEDB88320
    jmp .next_bit
.no_poly:
    shr edx, 1
.next_bit:
    dec si
    jnz .bit_loop

    inc di
    dec ecx
    jmp .byte_loop

.done:
    pop di
    pop si
    pop ecx
    pop eax
    ret

read_deed:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    mov eax, [ground_start_lba]
    mov [dap_deed + 8], eax
    mov eax, [ground_start_lba + 4]
    mov [dap_deed + 12], eax
    mov si, dap_deed
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc .try_tail

    mov al, DEED_ROLE_HEAD
    call validate_deed
    jnc .ok

.try_tail:
    mov si, msg_deed_head_bad
    call print_string_16

    mov eax, [ground_sectors]
    shr eax, TAGFS_SECTORS_PER_BLOCK_LOG2
    dec eax
    shl eax, TAGFS_SECTORS_PER_BLOCK_LOG2
    add eax, [ground_start_lba]
    mov edx, [ground_start_lba + 4]
    adc edx, 0
    mov [dap_deed + 8], eax
    mov [dap_deed + 12], edx
    mov si, dap_deed
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc .fail

    mov al, DEED_ROLE_TAIL
    call validate_deed
    jc .fail

    mov si, msg_deed_from_tail
    call print_string_16

.ok:
    clc
    jmp .out
.fail:
    stc
.out:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

validate_deed:
    push bx
    push cx
    push dx
    push si
    push es
    push eax

    mov [deed_want_role], al

    mov ax, DEED_SEG
    mov es, ax

    cmp dword [es:DEED_OFF_MAGIC], DEED_MAGIC_LO
    jne .bad
    cmp dword [es:DEED_OFF_MAGIC + 4], DEED_MAGIC_HI
    jne .bad

    mov ax, [es:DEED_OFF_PROLOGUE_BYTES]
    cmp ax, DEED_PROLOGUE_BYTES
    jb .bad
    cmp ax, DEED_BYTES
    ja .bad
    mov cx, [es:DEED_OFF_STAMP_BYTES]
    mov bx, DEED_BYTES
    sub bx, ax
    cmp cx, bx
    ja .bad

    add cx, ax
    call deed_crc32
    cmp edx, [es:DEED_OFF_CRC32]
    jne .bad_crc

    mov al, [deed_want_role]
    movzx eax, al
    cmp eax, [es:DEED_OFF_ROLE]
    jne .bad_role

    pop eax
    clc
    jmp .out

.bad_crc:
    mov si, msg_deed_crc
    call print_string_16
    jmp .fail
.bad_role:
    mov si, msg_deed_role
    call print_string_16
    jmp .fail
.bad:
    mov si, msg_deed_not_one
    call print_string_16
.fail:
    pop eax
    stc
.out:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    ret

deed_crc32:
    push eax
    push bx
    push cx
    push si

    mov edx, 0xFFFFFFFF
    xor bx, bx
.byte_loop:
    cmp bx, cx
    jae .done

    xor eax, eax
    cmp bx, DEED_OFF_CRC32
    jb .take
    cmp bx, DEED_OFF_CRC32 + 4
    jb .have
.take:
    mov al, [es:bx]
.have:
    xor edx, eax
    mov si, 8
.bit_loop:
    test edx, 1
    jz .no_poly
    shr edx, 1
    xor edx, 0xEDB88320
    jmp .next_bit
.no_poly:
    shr edx, 1
.next_bit:
    dec si
    jnz .bit_loop

    inc bx
    jmp .byte_loop

.done:
    not edx
    pop si
    pop cx
    pop bx
    pop eax
    ret

find_kernel_in_deed:
    push ax
    push bx
    push cx
    push si
    push es

    mov ax, DEED_SEG
    mov es, ax

    mov si, [es:DEED_OFF_PROLOGUE_BYTES]
    mov cx, [es:DEED_OFF_STAMP_BYTES]

.walk:
    cmp cx, 4
    jb .missing

    mov ax, [es:si + 2]
    add ax, 4 + 3
    and ax, 0xFFFC
    cmp cx, ax
    jb .missing

    cmp word [es:si], VOLUME_STAMP_BOOT
    je .found

    sub cx, ax
    add si, ax
    jmp .walk

.found:
    cmp word [es:si + 2], 12
    jb .missing
    add si, 4

    mov eax, [es:si + BOOT_KERNEL_BLOCK]
    mov [kernel_start_block], eax
    mov eax, [es:si + BOOT_KERNEL_BLOCKS]
    mov [kernel_block_count], eax
    mov eax, [es:si + BOOT_KERNEL_BYTES]
    mov [kernel_size_bytes], eax

    cmp dword [kernel_start_block], 0
    je .missing
    cmp dword [kernel_block_count], 0
    je .missing
    cmp dword [kernel_block_count], KERNEL_MAX_BLOCKS
    ja .missing

    xor ax, ax
    mov es, ax
    clc
    jmp .out

.missing:
    xor ax, ax
    mov es, ax
    stc
.out:
    pop es
    pop si
    pop cx
    pop bx
    pop ax
    ret

validate_kernel_magic:
    push eax
    push edi

    xor eax, eax
    mov es, ax
    mov edi, KERNEL_RUN_ADDR
    a32 mov eax, [es:edi + 2]
    cmp eax, KERNEL_HDR_MAGIC
    jne .bad_magic
    a32 mov ax, [es:edi + 6]
    cmp ax, KERNEL_HDR_MAGIC_HI
    jne .bad_magic

    a32 mov eax, [es:edi + 8]
    cmp eax, KERNEL_HDR_VERSION
    jne .bad_version

    pop edi
    pop eax
    ret

.bad_magic:
    mov si, msg_kernel_bad_magic
    call print_string_16
    cli
    hlt
    jmp $

.bad_version:
    mov si, msg_kernel_bad_version
    call print_string_16
    cli
    hlt
    jmp $

tagfs_load_kernel_file:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov eax, [kernel_start_block]
    shl eax, TAGFS_SECTORS_PER_BLOCK_LOG2
    add eax, [ground_start_lba]
    mov edx, [ground_start_lba + 4]
    adc edx, 0
    mov [kernel_load_sector], eax
    mov [kernel_load_sector_hi], edx

    mov eax, [kernel_block_count]
    shl eax, TAGFS_SECTORS_PER_BLOCK_LOG2
    mov [kernel_load_sectors], eax

    mov ecx, [kernel_load_sectors]
    mov ebx, [kernel_load_sector]
    mov edi, KERNEL_RUN_ADDR

.load_loop:
    mov eax, 64
    cmp ecx, 64
    jae .do_read
    mov eax, ecx

.do_read:
    push ecx
    push eax

    mov [dap_kernel_chunk + 2], ax
    mov word [dap_kernel_chunk + 4], 0x0000
    mov word [dap_kernel_chunk + 6], KERNEL_BOUNCE_SEG
    mov [dap_kernel_chunk + 8], ebx
    push eax
    mov eax, [kernel_load_sector_hi]
    mov [dap_kernel_chunk + 12], eax
    pop eax

    mov si, dap_kernel_chunk
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc  .read_failed

    pop eax
    pop ecx

    push ecx
    movzx ecx, ax
    shl ecx, 7
    mov esi, KERNEL_BOUNCE_ADDR
    cld
    a32 rep movsd
    pop ecx

    movzx eax, ax
    add ebx, eax
    adc dword [kernel_load_sector_hi], 0
    sub ecx, eax

    mov eax, edi
    sub eax, KERNEL_RUN_ADDR
    cmp eax, KERNEL_MAX_SIZE
    jae .size_error

    test ecx, ecx
    jnz .load_loop

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

.read_failed:
    pop eax
    pop ecx
    jmp .load_error

.size_error:
    mov si, msg_kernel_too_large
    call print_string_16
    jmp .unwind

.load_error:
    mov si, msg_kernel_load_error_pre
    call print_string_16
    call print_disk_status
.unwind:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    stc
    ret

detect_memory_e820:
    mov si, msg_detecting_memory
    call print_string_16

    mov ax, E820_SEG
    mov es, ax
    xor di, di
    mov cx, E820_REGION_WORDS
    xor ax, ax
    rep stosw

    xor ebx, ebx
    mov edx, 0x534D4150
    mov ax, E820_SEG
    mov es, ax
    mov di, 4
    xor bp, bp

.e820_loop:
    mov ax, E820_SEG
    mov es, ax

    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .e820_end

    cmp eax, 0x534D4150
    jne .e820_fail

    cmp ecx, 20
    jl .skip_entry

    mov eax, [es:di + 8]
    or  eax, [es:di + 12]
    jz .skip_entry

    cmp ecx, 24
    jb .accept_entry
    test byte [es:di + 20], 1
    jz .skip_entry

.accept_entry:
    inc bp
    add di, 24

    cmp bp, E820_MAX_ENTRIES
    jae .e820_done

.skip_entry:
    test ebx, ebx
    jnz .e820_loop

.e820_end:
    test bp, bp
    jz .e820_fail

.e820_done:

    xor ax, ax
    mov es, ax

    mov [E820_COUNT_ADDR], bp

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

    mov ax, E820_SEG
    mov es, ax
    mov di, 4

    mov dword [es:di], 0x00000000
    mov dword [es:di+4], 0x00000000
    mov dword [es:di+8], 0x0009FC00
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0
    add di, 24

    xor cx, cx
    xor dx, dx
    mov ax, 0xE801
    int 0x15
    jc .try_int88

    test ax, ax
    jnz .e801_ax_ok
    mov ax, cx
.e801_ax_ok:
    test bx, bx
    jnz .e801_bx_ok
    mov bx, dx
.e801_bx_ok:

    movzx eax, ax
    shl eax, 10
    mov dword [es:di], 0x00100000
    mov dword [es:di+4], 0x00000000
    mov [es:di+8], eax
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0
    add di, 24

    test bx, bx
    jz .e801_done
    movzx ebx, bx
    shl ebx, 16
    mov dword [es:di], 0x01000000
    mov dword [es:di+4], 0x00000000
    mov [es:di+8], ebx
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0
    add di, 24

    xor ax, ax
    mov es, ax
    mov word [E820_COUNT_ADDR], 3
    mov word [E820_SIZE_ADDR], 72
    mov si, msg_memory_fallback
    call print_string_16
    ret

.e801_done:
    xor ax, ax
    mov es, ax
    mov word [E820_COUNT_ADDR], 2
    mov word [E820_SIZE_ADDR], 48
    mov si, msg_memory_fallback
    call print_string_16
    ret

.try_int88:
    mov ah, 0x88
    int 0x15
    jc .memory_fail
    movzx eax, ax
    shl eax, 10
    mov dword [es:di], 0x00100000
    mov dword [es:di+4], 0x00000000
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

DRD_CHUNK   equ 64
DRD_RETRIES equ 5

disk_read_dap:
    pushad

    mov [drd_drive], dl

    mov ax, [si + 2]
    mov [drd_left], ax
    mov eax, [si + 8]
    mov [drd_pkt + 8], eax
    mov eax, [si + 12]
    mov [drd_pkt + 12], eax

    mov ax, [si + 4]
    mov bx, ax
    and bx, 0x000F
    mov [drd_pkt + 4], bx
    shr ax, 4
    add ax, [si + 6]
    mov [drd_pkt + 6], ax

    mov byte [drd_pkt + 0], 0x10
    mov byte [drd_pkt + 1], 0

    mov al, [drd_probed_for]
    cmp al, dl
    je .probed
    mov [drd_probed_for], dl
    mov byte [drd_edd], 0

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drd_drive]
    int 0x13
    jc .probe_geom
    cmp bx, 0xAA55
    jne .probe_geom
    test cl, 1
    jz .probe_geom
    mov byte [drd_edd], 1

.probe_geom:
    push es
    xor ax, ax
    mov es, ax
    xor di, di
    mov ah, 0x08
    mov dl, [drd_drive]
    int 0x13
    pop es
    jc .no_geom
    movzx ax, cl
    and ax, 0x3F
    mov [drd_spt], ax
    movzx ax, dh
    inc ax
    mov [drd_heads], ax
    jmp .probed
.no_geom:
    mov word [drd_spt], 0
    mov word [drd_heads], 0

.probed:
    mov byte [drd_err], 0xFF
    mov byte [drd_try], DRD_RETRIES

.next:
    cmp word [drd_left], 0
    je .ok

    cmp byte [drd_edd], 1
    je .via_edd

    cmp word [drd_spt], 0
    je .exhausted
    cmp dword [drd_pkt + 12], 0
    jne .exhausted

    mov eax, [drd_pkt + 8]
    xor edx, edx
    movzx ecx, word [drd_spt]
    div ecx
    inc dl
    mov bl, dl
    xor edx, edx
    movzx ecx, word [drd_heads]
    div ecx
    mov dh, dl
    cmp eax, 1023
    ja .exhausted

    mov ch, al
    mov cl, ah
    shl cl, 6
    or  cl, bl
    mov dl, [drd_drive]
    mov ax, [drd_pkt + 6]
    mov es, ax
    mov bx, [drd_pkt + 4]
    mov ax, 0x0201
    int 0x13
    jc .retry
    cmp al, 1
    jne .short
    mov ax, 1
    jmp .advance

.via_edd:
    mov ax, [drd_left]
    cmp ax, DRD_CHUNK
    jbe .asking
    mov ax, DRD_CHUNK
.asking:
    mov [drd_asked], ax
    mov [drd_pkt + 2], ax
    mov si, drd_pkt
    mov ah, 0x42
    mov dl, [drd_drive]
    int 0x13
    jc .retry
    mov ax, [drd_pkt + 2]
    test ax, ax
    jz .short
    cmp ax, [drd_asked]
    ja .short

.advance:
    sub [drd_left], ax
    movzx ecx, ax
    add [drd_pkt + 8], ecx
    adc dword [drd_pkt + 12], 0
    shl cx, 5
    add [drd_pkt + 6], cx
    mov byte [drd_try], DRD_RETRIES
    jmp .next

.short:
    xor ah, ah
.retry:
    mov [drd_err], ah
    dec byte [drd_try]
    jz .exhausted
    xor ax, ax
    mov dl, [drd_drive]
    int 0x13
    jmp .next

.exhausted:
    cmp byte [drd_edd], 1
    jne .fail
    mov byte [drd_edd], 0
    mov byte [drd_try], DRD_RETRIES
    jmp .next

.ok:
    call restore_unreal_mode
    popad
    clc
    ret
.fail:
    call restore_unreal_mode
    popad
    stc
    ret

print_disk_status:
    mov si, msg_bios_status
    call print_string_16
    mov al, [drd_err]
    call print_hex8
    mov si, msg_crlf
    call print_string_16
    ret

print_hex8:
    push bx
    push ax
    shr al, 4
    call .nibble
    pop ax
    and al, 0x0F
    call .nibble
    pop bx
    ret
.nibble:
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    ret

align 4
drd_pkt:            times 16 db 0
drd_left:           dw 0
drd_asked:          dw 0
drd_spt:            dw 0
drd_heads:          dw 0
drd_drive:          db 0
drd_probed_for:     db 0xFF
drd_edd:            db 0
drd_try:            db 0
drd_err:            db 0xFF
[BITS 32]

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

setup_paging:
    call calculate_identity_map_size

    push ecx

    mov edi, [dynamic_pt_base]
    mov ecx, 8192
    xor eax, eax
    rep stosd

    pop ecx
    push ecx

    mov esi, [dynamic_pt_base]
    mov eax, esi
    add eax, 0x1000 + 3
    mov [esi], eax
    mov dword [esi + 4], 0x00000000

    mov eax, ecx
    add eax, 511
    shr eax, 9
    mov ebx, eax

    xor edx, edx
.setup_pdpt:
    mov eax, edx
    shl eax, 12
    add eax, [dynamic_pt_base]
    add eax, 0x2000
    or eax, 3
    mov edi, [dynamic_pt_base]
    add edi, 0x1000
    lea edi, [edi + edx*8]
    mov [edi], eax
    mov dword [edi+4], 0

    inc edx
    cmp edx, ebx
    jb .setup_pdpt

    mov ebp, [guard1_base]
    shr ebp, 21
    mov esi, [guard2_base]
    shr esi, 21

    mov edi, [dynamic_pt_base]
    add edi, 0x2000
    mov eax, 0x000083
    xor edx, edx
    pop ecx

.fill_pd:
    cmp edx, ebp
    je .guard_entry
    cmp edx, esi
    je .guard_entry
    mov [edi], eax
    mov dword [edi+4], 0
    jmp .next_pd
.guard_entry:
    mov dword [edi], 0
    mov dword [edi+4], 0
.next_pd:
    add eax, 0x200000
    add edi, 8
    inc edx
    loop .fill_pd


    mov esi, [dynamic_pt_base]
    add esi, 0x2000
    mov edi, [dynamic_pt_base]
    add edi, 0x7000
    mov ecx, 16
.copy_pd_high:
    mov eax, [esi]
    mov [edi], eax
    mov eax, [esi+4]
    mov [edi+4], eax
    add esi, 8
    add edi, 8
    dec ecx
    jnz .copy_pd_high

    mov edi, [dynamic_pt_base]
    add edi, 0x6000
    mov eax, [dynamic_pt_base]
    add eax, 0x7000
    or eax, 3
    mov [edi + 510*8], eax
    mov dword [edi + 510*8 + 4], 0

    mov edi, [dynamic_pt_base]
    mov eax, [dynamic_pt_base]
    add eax, 0x6000
    or eax, 3
    mov [edi + 511*8], eax
    mov dword [edi + 511*8 + 4], 0

    ret

enable_long_mode:
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    mov eax, [dynamic_pt_base]
    mov cr3, eax

    xor esi, esi
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_nx
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 20)
    jz .no_nx
    mov esi, (1 << 11)
.no_nx:

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    or eax, esi
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ret

align 8
gdt_start:
    dq 0x0000000000000000

    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00

    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x9A
    db 0x20
    db 0x00

    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x92
    db 0x00
    db 0x00

gdt_end:

align 4
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

align 8
unreal_gdt_start:
    dq 0x0000000000000000

    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0x00
    db 0x00

    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

unreal_gdt_end:

align 4
unreal_gdt_descriptor:
    dw unreal_gdt_end - unreal_gdt_start - 1
    dd unreal_gdt_start

align 4
dap_sector0:
    db 0x10, 0
    dw 1
    dw 0x0000
    dw MBR_SEG
    dq 0

align 4
dap_gpt:
    db 0x10, 0
    dw 1
    dw 0x0000
    dw GPT_SCRATCH_SEG
    dq 0

align 4
dap_deed:
    db 0x10, 0
    dw DEED_SECTORS
    dw 0x0000
    dw DEED_SEG
    dq 0

align 4
dap_kernel_chunk:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x1000
    dq 0

align 4
boot_drive_saved:       db 0

boarding_loader_name:   db 'stage2', 0, 0, 0, 0, 0, 0

align 4
dap_work_lba:           dd 0
dap_work_count:         dd 0
dap_work_seg:           dw 0
dap_work_off:           dw 0

align 4
dynamic_pt_base:        dd 0
dynamic_stack_base:     dd 0
guard1_base:            dd 0
guard2_base:            dd 0

kernel_file_id:         dw 0
kernel_start_block:     dd 0
kernel_block_count:     dd 0
align 4
ground_start_lba:       dq 0
ground_sectors:         dd 0
deed_want_role:         db 0

align 4
gpt_header_bytes:       dd 0
gpt_want_crc:           dd 0
gpt_entry_lba:          dq 0
gpt_entry_count:        dd 0
gpt_entry_bytes:        dd 0
gpt_array_crc:          dd 0
gpt_array_bytes:        dd 0
gpt_window_bytes:       dd 0
gpt_found:              dd 0
gpt_cand_first:         dq 0
gpt_cand_last:          dq 0

BOXOS_TYPE_GUID_0       equ 0xcf8ae49a
BOXOS_TYPE_GUID_1       equ 0x4959d26a
BOXOS_TYPE_GUID_2       equ 0xc9716f9a
BOXOS_TYPE_GUID_3       equ 0xebc9e032
kernel_load_sector:     dd 0
kernel_load_sector_hi:  dd 0
kernel_load_sectors:    dd 0
kernel_size_bytes:      dd 0
kernel_loaded_bytes:    dd 0

msg_stage2_start      db 'BoxKernel Stage2 Started', 13, 10, 0
msg_a20_enabled       db '[OK] A20 line enabled', 13, 10, 0
msg_a20_fatal         db '[FATAL] A20 enable failed — system halted', 13, 10, 0
msg_detecting_memory  db 'Detecting memory (E820)...', 13, 10, 0
msg_e820_success      db '[OK] E820 memory map created', 13, 10, 0
msg_e820_fail         db '[WARN] E820 failed, using fallback', 13, 10, 0
msg_memory_fallback   db '[OK] Fallback memory detection', 13, 10, 0
msg_memory_error      db '[ERROR] Memory detection failed!', 13, 10, 0
msg_unreal_mode       db '[OK] Unreal Mode (4GB addressing) active', 13, 10, 0
msg_kernel_too_large  db '[ERROR] Kernel exceeds 32MB limit!', 13, 10, 0
msg_loading_tagfs     db 'Finding the volume and loading its kernel...', 13, 10, 0
msg_kernel_loaded_tagfs db '[OK] Kernel loaded to 0x100000 via Unreal Mode', 13, 10, 0
msg_no_ground         db '[ERROR] This medium says nothing about where BoxOS lives', 13, 10, 0
msg_no_table          db '[ERROR] No partition table on this medium', 13, 10, 0
msg_no_boxos_partition db '[ERROR] A partition table with no BoxOS partition in it', 13, 10, 0
msg_gpt_no_header     db '[ERROR] This disk says it is a GPT and would not give up its header', 13, 10, 0
msg_gpt_absent        db '[ERROR] A protective MBR with no GPT behind it', 13, 10, 0
msg_gpt_header_len    db '[ERROR] Its GPT header states a length that cannot be one', 13, 10, 0
msg_gpt_header_crc    db '[ERROR] Its GPT header does not match its own checksum', 13, 10, 0
msg_gpt_array_shape   db '[ERROR] Its GPT entry table is not a shape this loader reads', 13, 10, 0
msg_gpt_no_array      db '[ERROR] Its GPT entries would not read', 13, 10, 0
msg_gpt_array_crc     db '[ERROR] Its GPT entries do not match their own checksum', 13, 10, 0
msg_gpt_not_ours      db '[ERROR] Its GPT has no BoxOS partition in it', 13, 10, 0
msg_gpt_too_long      db '[ERROR] That BoxOS partition is longer than this loader can address', 13, 10, 0
msg_ground_too_small  db '[ERROR] The BoxOS partition is too small to hold a deed', 13, 10, 0
msg_no_deed           db '[ERROR] Neither copy of this volume deed can be read', 13, 10, 0
msg_deed_head_bad     db '[WARN] The deed at the head is unreadable; trying the far end', 13, 10, 0
msg_deed_from_tail    db '[OK] Read the deed from the far end of the volume', 13, 10, 0
msg_deed_not_one      db '[ERROR] What is at this sector is not a deed', 13, 10, 0
msg_deed_crc          db '[ERROR] The deed does not match its own checksum', 13, 10, 0
msg_deed_role         db '[ERROR] The deed is the wrong end of the volume', 13, 10, 0
msg_kernel_not_found  db '[ERROR] This volume does not say where its kernel is', 13, 10, 0
msg_kernel_load_error_pre db '[ERROR] Kernel file load failed!', 13, 10, 0
msg_bios_status       db '        last BIOS disk status: 0x', 0
msg_crlf              db 13, 10, 0
msg_long_mode_ok      db '[OK] CPU supports 64-bit mode', 13, 10, 0
msg_no_cpuid          db '[ERROR] CPUID not supported!', 13, 10, 0
msg_no_long_mode      db '[ERROR] 64-bit mode not supported!', 13, 10, 0
msg_entering_protected db 'Entering protected mode...', 13, 10, 0
msg_kernel_bad_magic      db '[FATAL] Kernel header magic mismatch — corrupted load!', 13, 10, 0
msg_kernel_bad_version    db '[FATAL] Kernel header version mismatch — rebuild required!', 13, 10, 0


%if __?BITS?__ != 32
  %error "stage2 must end in BITS 32: the protected-mode block lost its directive"
%endif

%if ($ - $$) > STAGE2_SECTORS * 512
  %error "stage2 exceeds its sector window: stage1 would load a truncated image"
%endif
%if 0x8000 + STAGE2_SECTORS * 512 > BOOT_INFO_ADDR
  %error "stage2's load window runs into boot_info"
%endif
%if BOOT_INFO_ADDR + 256 > MBR_ADDR
  %error "boot_info runs into the partition-table buffer"
%endif
%if MBR_ADDR + TAGFS_SECTOR_SIZE > BOARDING_PASS_ADDR
  %error "the partition-table buffer runs into the boarding pass"
%endif
%if BOARDING_PASS_ADDR + BOARDING_PASS_BYTES > DEED_ADDR
  %error "the boarding pass runs into the deed buffer"
%endif
%if DEED_ADDR + DEED_BYTES > KERNEL_BOUNCE_ADDR
  %error "the deed buffer runs into the bounce buffer"
%endif

%if E820_MAP_ADDR + E820_MAX_ENTRIES * 24 > 0x1200
  %error "the E820 map has grown into stage1's scratch at 0x1200"
%endif