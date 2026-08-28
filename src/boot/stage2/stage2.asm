[BITS 16]
[ORG 0x8000]
DEFAULT ABS

; Memory map (dynamic layout — addresses computed from actual kernel size).
; Every range below is claimed by exactly one owner, and the ones that follow
; this image start ABOVE it. That was not true until 2026-08-23: boot_info sat
; at 0x9000 while this binary had grown past 0x904F, so the handoff structure
; was written into the loader's own message strings, and the TagFS superblock
; buffer had 177 bytes of clearance left. Both were harmless by accident — the
; strings are never printed again, and the image had not yet grown that last
; inch — and neither was harmless by construction. Stage1 also loads all
; sixteen sectors unconditionally, so 0x8000..0x9FFF is written in full
; whatever this binary's actual size is; the window is stage2's, entire.
; The assertions at the bottom of this file enforce the map rather than
; describe it.
; 0x00500..0x01103    - E820 memory map (header + up to 128 x 24-byte entries)
; 0x01200..0x01217    - Stage1 scratch (drive, EDD flag, geometry, DAP)
; 0x07000             - 16-bit stack top, grows down
; 0x07C00             - Stage1 (512 bytes)
; 0x08000..0x09FFF    - Stage2 (16 sectors — the whole window, always loaded)
; 0x0A000             - Boot info for kernel (structured, versioned)
; 0x0A200             - Sector 0 of the medium: its partition table (512 bytes)
; 0x0A600             - Boarding pass for kernel (512 bytes, stamped)
; 0x0B000..0x0BFFF    - The volume's Deed (4096 bytes — one whole block)
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
; Guard regions are full 2 MB so we can mark them non-present at PD-entry
; granularity (the identity map uses 2 MB pages). A 4 KB guard would force
; us to drop the entire surrounding 2 MB to 4 KB pages just to clear one
; entry — far costlier than the few MB of physical address space we burn
; for the alignment.
GUARD_PAGE_SIZE       equ 0x200000     ; 2 MB (matches PD-entry granularity)
BOOT_STACK_SIZE       equ 0x10000      ; 64KB boot stack (stack grows downward)

; E820 layout at 0x500 (out of BDA which ends at 0x4FF):
;   0x500: count (uint16)   — number of valid E820 entries
;   0x502: size  (uint16)   — byte size of the map (count * 24)
;   0x504: map entries      — 24-byte e820_entry_t records
E820_COUNT_ADDR       equ 0x500
E820_SIZE_ADDR        equ 0x502
E820_MAP_ADDR         equ 0x504
E820_MAX_ENTRIES      equ 128           ; must match E820_MAX_ENTRIES in e820.h
E820_SEG              equ E820_COUNT_ADDR >> 4   ; 0x0050:0000 = 0x500
; Words to wipe before the firmware fills the map. It used to be a flat 1024,
; which covers 2048 bytes — but the map reaches 0x504 + 128*24 = 0x1104, so
; from the 34th entry onward the loader handed the kernel whatever had been in
; that memory for any field the firmware chose not to write.
E820_REGION_WORDS     equ (4 + E820_MAX_ENTRIES * 24 + 1) / 2

STAGE2_SIGNATURE      equ 0x2907

; Layout facts the image build owns — where stage2 is written, how much of it
; stage1 loads, and where the handoff block goes. They arrive by -D and are
; deliberately given no default: a default is a second place the number is
; written down, and the whole point of this session was finding the fourth.
%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must come from the build (-DSTAGE2_SECTORS=...)"
%endif
%ifndef BOARDING_PASS_ADDR
  %error "BOARDING_PASS_ADDR must come from the build (-DBOARDING_PASS_ADDR=...)"
%endif
%ifndef BOOT_INFO_ADDR
  %error "BOOT_INFO_ADDR must come from the build (-DBOOT_INFO_ADDR=...)"
%endif

; boot_info structure constants (shared contract with kernel)
BOOT_INFO_MAGIC       equ 0x42583031     ; "BX01" — BoxOS boot info v1
BOOT_INFO_VERSION     equ 1

; The Boarding Pass — what this loader tells the kernel about its own arrival,
; as opposed to boot_info, which describes the machine. Layout in
; src/include/boarding_pass.h; the numbers below are that layout and the
; kernel's static assertions are the other half of the agreement.
BOARDING_PASS_MAGIC   equ 0x53415042     ; "BPAS"
BOARDING_PASS_VERSION equ 1
BOARDING_PASS_BYTES   equ 512
BOARDING_HDR_BYTES    equ 16
BOARDING_STAMP_VOLUME equ 1
BOARDING_STAMP_MEDIUM equ 2
BOARDING_STAMP_LOADER equ 3
BOARDING_STAMP_SEAL   equ 4
BOARDING_FIRMWARE_BIOS equ 0
; ---------------------------------------------------------------------------
; The Deed, by offset.
;
; This loader used to read a superblock at absolute sector 1034 and take the
; volume's identity from byte 88 of it. Both numbers were spelled out here, in
; two kernel headers and in the UEFI loader, and a volume could therefore exist
; in exactly one place on any medium.
;
; Now the medium's partition table says where the volume's ground is, and the
; Deed at the head of that ground says what is on it. An assembler cannot ask a
; C structure where its fields are, so these offsets are that structure written
; out — and src/include/volume_deed.h carries the static assertions that stop
; the two from drifting.
; ---------------------------------------------------------------------------
DEED_OFF_MAGIC          equ 0
DEED_OFF_PROLOGUE_BYTES equ 8
DEED_OFF_STAMP_BYTES    equ 10
DEED_OFF_CRC32          equ 12
DEED_OFF_UUID           equ 16
DEED_OFF_SECTORS        equ 32
DEED_OFF_TAIL_SECTOR    equ 40
DEED_OFF_ROLE           equ 48
DEED_PROLOGUE_BYTES     equ 52

DEED_MAGIC_LO           equ 0x44584F42  ; 'B','O','X','D'
DEED_MAGIC_HI           equ 0x00444545  ; 'E','E','D',0
DEED_ROLE_HEAD          equ 1
DEED_ROLE_TAIL          equ 2
DEED_SECTORS            equ 8
DEED_BYTES              equ DEED_SECTORS * 512

; The one stamp this loader needs: where the kernel is, in the volume's blocks.
VOLUME_STAMP_BOOT       equ 4
BOOT_KERNEL_BLOCK       equ 0
BOOT_KERNEL_BLOCKS      equ 4
BOOT_KERNEL_BYTES       equ 8

; The MBR, by offset. Four sixteen-byte entries at 446, then 0xAA55.
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

; ---------------------------------------------------------------------------
; GPT, by offset. UEFI 2.10 §5.3. The header states its own length, which is
; what its checksum covers — not a fixed 92, because a later revision may be
; longer and a reader that assumes the length gets the sum wrong on hardware
; it has never seen.
; ---------------------------------------------------------------------------
GPT_HEADER_LBA          equ 1
GPT_SIG_LO              equ 0x20494645  ; 'E','F','I',' '
GPT_SIG_HI              equ 0x54524150  ; 'P','A','R','T'
GPT_OFF_HEADER_BYTES    equ 0x0C
GPT_OFF_HEADER_CRC      equ 0x10
GPT_OFF_ENTRY_LBA       equ 0x48
GPT_OFF_ENTRY_COUNT     equ 0x50
GPT_OFF_ENTRY_BYTES     equ 0x54
GPT_OFF_ENTRY_CRC       equ 0x58
GPT_HEADER_MIN_BYTES    equ 92
GPT_ENTRY_MIN_BYTES     equ 128
; The spec reserves 16 KiB for the array and every tool in use writes exactly
; that. This is the ceiling on what will be read, so a header stating an absurd
; array cannot talk this loader into an unbounded scan.
GPT_ARRAY_MAX_BYTES     equ 65536

GPT_ENTRY_OFF_TYPE      equ 0
GPT_ENTRY_OFF_FIRST     equ 0x20
GPT_ENTRY_OFF_LAST      equ 0x28

; The scratch the GPT is read through: one 4 KiB window, reused for the header
; and then for the entry array a chunk at a time. The array is never held whole
; — its checksum is taken as it streams past, which is what lets a 16 KiB table
; be verified by a loader with 4 KiB to spare.
GPT_SCRATCH_ADDR        equ 0xC000
GPT_SCRATCH_SEG         equ GPT_SCRATCH_ADDR >> 4
GPT_SCRATCH_BYTES       equ 4096
GPT_SCRATCH_SECTORS     equ GPT_SCRATCH_BYTES / 512
DEED_ADDR               equ 0xB000
DEED_SEG                equ DEED_ADDR >> 4

KERNEL_HDR_MAGIC        equ 0x4E52454B  ; "KERN" little-endian
KERNEL_HDR_MAGIC_HI     equ 0x4C45      ; "EL" little-endian
KERNEL_HDR_VERSION      equ 1           ; must match kernel_entry.asm dd at offset +8

; TagFS layout constants — mirror src/boot/uefi/tagfs_boot.h and
; src/kernel/tagfs/tagfs_constants.h. NASM can't include C headers, so we
; redefine here. Compile-time check via the assert at the bottom of this
; file would be ideal, but NASM lacks _Static_assert; if any of these
; change in C, also update here. KERNEL_MAX_BLOCKS is derived to keep the
; bound consistent with KERNEL_MAX_SIZE.
TAGFS_BLOCK_SIZE              equ 4096
TAGFS_SECTOR_SIZE             equ 512
TAGFS_SECTORS_PER_BLOCK       equ TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE   ; 8
TAGFS_SECTORS_PER_BLOCK_LOG2  equ 3                                      ; 8 = 1<<3
KERNEL_MAX_BLOCKS             equ KERNEL_MAX_SIZE / TAGFS_BLOCK_SIZE     ; 8192

; Stage2 binary layout at offset 0 (= load address 0x8000):
;   +0: jmp short past_sig    (2 bytes: EB 02) — branches over the signature
;   +2: STAGE2_SIGNATURE word  (read by stage1 to verify it loaded a real
;                               stage2 binary, not garbage from a bad sector)
;   +4: real entry point — first instruction the CPU actually executes after
;       stage1's `jmp 0x0000:0x8000` lands at offset 0 and falls through.
;
; Without the jump-over-signature, the CPU would interpret the signature
; bytes 0x07 0x29 as instructions (`pop es; sub dx, di`) and clobber both
; ES and DL before any of our real code runs. DL carries the BIOS boot
; drive number — losing it breaks the entire raw-PIO disk path. QEMU
; happened to land on register values that survived the sub-by-accident;
; Bochs and various real-HW BIOSes leave different state and the boot
; would silently fail to find the volume at all.
jmp short past_sig
dw STAGE2_SIGNATURE
past_sig:

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
    mov sp, 0x7000      ; safe stack: below stage1 (0x7C00), above E820 data (~0x1104)

    ; Force VGA text mode 3 (80x25 color) before any output.
    ; On real hardware, BIOS does not guarantee a known video state at boot.
    ; INT 10h also re-initialises the cursor, clears the screen — deterministic start.
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

    ; Round kernel_end up to 2 MB so the guard sits on a clean PD boundary.
    add eax, 0x1FFFFF
    and eax, 0xFFE00000
    mov [guard1_base], eax

    ; guard1 (2 MB, marked P=0 in setup_paging) → page tables
    add eax, GUARD_PAGE_SIZE
    mov [dynamic_pt_base], eax

    ; Page tables (32 KB) → round up to 2 MB so the next guard is PD-aligned.
    add eax, PAGE_TABLE_SIZE
    add eax, 0x1FFFFF
    and eax, 0xFFE00000
    mov [guard2_base], eax

    ; guard2 (2 MB, P=0) → boot stack (64 KB)
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

    ; Use dynamically computed stack (mov esp zero-extends into RSP)
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

    ; kernel_end = the *true* end of memory the kernel will occupy at
    ; runtime — i.e. including BSS. The linker bakes that value into the
    ; kernel header at offset +12 (`_kernel_phys_end`), and that's the
    ; same value compute_dynamic_layout already trusted to place the
    ; page tables. Reporting `KERNEL_RUN_ADDR + kernel_loaded_bytes`
    ; here used to leak the wrong number out of BIOS boot — the kernel
    ; saw a kernel_end that stopped before BSS. UEFI's tagboot already
    ; reports the BSS-inclusive value, so reporting it here too keeps
    ; both boot paths telling the same story to PMM.
    mov eax, [KERNEL_RUN_ADDR + 12]
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

; ---------------------------------------------------------------------------
; The Boarding Pass.
;
; Four stamps, and the one that matters is the first: the identity of the
; volume this kernel was just read out of. It is sitting in the Deed this
; loader read at the start, sixteen bytes at DEED_OFF_UUID.
;
; Without it the kernel reaches long mode with no way to ask the firmware which
; device it booted from, and the Boardroom picks a volume by a rule — "the
; removable medium wins" — which is wrong on any machine carrying BoxOS on an
; internal disk with a flash drive in a socket. This loader is the one part of
; the system that knows the answer.
;
; Written here, in long mode, immediately after boot_info and immediately
; before the jump: everything it needs is settled by this point and nothing
; runs between this and the kernel that could disturb it.
; ---------------------------------------------------------------------------
write_boarding_pass:
    ; Header
    mov dword [BOARDING_PASS_ADDR],     BOARDING_PASS_MAGIC     ; +0  magic
    mov word  [BOARDING_PASS_ADDR+4],   BOARDING_PASS_VERSION   ; +4  version
    mov word  [BOARDING_PASS_ADDR+6],   BOARDING_HDR_BYTES      ; +6  header_bytes
    mov word  [BOARDING_PASS_ADDR+8],   72                      ; +8  used_bytes
    mov word  [BOARDING_PASS_ADDR+10],  BOARDING_PASS_BYTES     ; +10 capacity
    mov word  [BOARDING_PASS_ADDR+12],  4                       ; +12 count
    mov word  [BOARDING_PASS_ADDR+14],  0                       ; +14 reserved

    ; Stamp 1 at +16: the volume, sixteen bytes out of its Deed.
    mov word  [BOARDING_PASS_ADDR+16],  BOARDING_STAMP_VOLUME
    mov word  [BOARDING_PASS_ADDR+18],  16
    mov rax, [DEED_ADDR + DEED_OFF_UUID]
    mov [BOARDING_PASS_ADDR+20], rax
    mov rax, [DEED_ADDR + DEED_OFF_UUID + 8]
    mov [BOARDING_PASS_ADDR+28], rax

    ; Stamp 2 at +36: the medium. The drive number is the one this loader
    ; actually used, which is not always the one the firmware first offered —
    ; there is a fallback to 0x80 above for firmware that hands out a handle it
    ; then does not honour, and the kernel should be told which one worked.
    mov word  [BOARDING_PASS_ADDR+36],  BOARDING_STAMP_MEDIUM
    mov word  [BOARDING_PASS_ADDR+38],  4
    mov byte  [BOARDING_PASS_ADDR+40],  BOARDING_FIRMWARE_BIOS
    mov al, [boot_drive_saved]
    mov byte  [BOARDING_PASS_ADDR+41],  al
    mov word  [BOARDING_PASS_ADDR+42],  0

    ; Stamp 3 at +44: who wrote this. On a board that boots through CSM one
    ; week and UEFI the next, that is a real question with nothing on the
    ; screen to answer it.
    mov word  [BOARDING_PASS_ADDR+44],  BOARDING_STAMP_LOADER
    mov word  [BOARDING_PASS_ADDR+46],  16
    mov rax, [boarding_loader_name]
    mov [BOARDING_PASS_ADDR+48], rax
    mov eax, [boarding_loader_name+8]
    mov [BOARDING_PASS_ADDR+56], eax
    mov word  [BOARDING_PASS_ADDR+60],  1                       ; major
    mov word  [BOARDING_PASS_ADDR+62],  0                       ; minor

    ; Stamp 4 at +64: the seal, and it is last because it covers everything
    ; ahead of it — the header, the three stamps, and its own kind/length pair.
    ; The rule and the reasoning live in src/include/boarding_pass.h; what is
    ; here is the same CRC-32 (ISO 3309) the Deed and the GPT are checked with.
    ;
    ; It cannot reuse crc32_chunk further down this file: that one addresses
    ; through ES:DI and belongs to the real-mode half, and this runs in long
    ; mode. Sixty-eight bytes, once per boot.
    mov word  [BOARDING_PASS_ADDR+64],  BOARDING_STAMP_SEAL
    mov word  [BOARDING_PASS_ADDR+66],  4

    mov esi, BOARDING_PASS_ADDR
    mov ecx, 68                         ; up to the seal's payload, not into it
    mov eax, 0xFFFFFFFF
.seal_byte:
    movzx edx, byte [rsi]
    xor eax, edx
    mov edx, 8
.seal_bit:
    shr eax, 1                          ; CF = the bit that just fell off
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

    ; Final verification: confirm A20 is actually enabled after all methods.
    ; If still disabled → fatal halt. Silent continue would corrupt memory at >1MB.
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
    mov ax, 0           ; must NOT use xor ax,ax — that sets ZF=1 and destroys the cmp result
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

; =============================================================
; Restore Unreal Mode (DS and ES descriptor caches to 4GB limit)
; MUST be called after any BIOS interrupt that may have reloaded
; DS or ES (INT 13h, INT 15h E820, etc.) — they all re-prime the
; descriptor cache with a real-mode 64KB limit, which breaks the
; 32-bit address overrides used by a32 rep movsd.
; Requires: unreal GDT already loaded by enter_unreal_mode.
; Clobbers: AX (low 16 bits of EAX), flags.
; =============================================================
restore_unreal_mode:
    cli

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, 0x10        ; flat 4GB data segment
    mov ds, ax
    mov es, ax

    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    jmp 0x0000:.rum_flush
.rum_flush:
    xor ax, ax          ; base = 0, but descriptor cache keeps 4GB limit
    mov ds, ax
    mov es, ax

    sti
    ret

load_kernel_tagfs:
    mov si, msg_loading_tagfs
    call print_string_16

    ; The firmware told stage1 which handle to use, and stage1 passed it here
    ; unchanged. Now that reads go through INT 13h — the same interface the
    ; firmware used to load us — that handle is right by construction.
    ;
    ; The old code toggled 0x80 <-> 0x81 here, on the theory that BIOSes
    ; "occasionally hand us the wrong drive number". That theory only made
    ; sense while reads bypassed the BIOS and had to name an IDE channel
    ; themselves; the number was never wrong, it just did not mean what the
    ; PIO path needed it to mean. The toggle also assumed DL was one of
    ; exactly two values, which stopped being true the moment stage1 started
    ; accepting the 0x00 that USB-FDD emulation legitimately reports — XOR 1
    ; would have turned it into 0x01, a handle belonging to nothing.
    ;
    ; One fallback survives, and only one: 0x80, the first hard disk by every
    ; convention there is, for firmware that hands out a handle it then does
    ; not honour.
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

; ---------------------------------------------------------------------------
; find_ground — where on this medium is the volume?
;
; Read sector 0 and take the first partition claimed for BoxOS. This is the
; same question the kernel asks of the same bytes (core/boardroom/ground.c),
; and asking it here is what replaced "the volume begins at sector 1034".
;
; MBR only. A GPT disk says so with a protective entry and is refused out loud
; rather than misread: verifying a GPT means two CRC32s over a 16 KiB entry
; array, and this loader runs in real mode with 3 KiB of window left. The UEFI
; loader, which is the one that boots a GPT disk in practice, reads both.
;
; Returns: CF=0 with [ground_start_lba] and [ground_sectors] set, CF=1 with a
;          reason already on screen.
; ---------------------------------------------------------------------------
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

    ; A protective entry anywhere in the four means the real table is a GPT,
    ; and the MBR beside it describes a disk that does not exist.
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

; ---------------------------------------------------------------------------
; find_ground_gpt — the same question of a GPT disk.
;
; Both checksums are verified, because both exist for a reason: a header that
; passes its own sum can still point at an entry array that was interrupted
; mid-write, and taking a partition out of that array is taking it out of
; whatever was there before.
;
; The array is streamed rather than held: one 4 KiB window at a time, with the
; checksum carried across chunks and the entries examined as they pass. A
; 16 KiB table is therefore verified by a loader that has 4 KiB to spare, and a
; table larger than any tool writes is refused rather than half-read.
;
; The candidate is remembered while the sum is still unknown and only ACCEPTED
; at the end, if the sum agrees — reading a partition out of an array that
; fails its checksum is the thing this is here to prevent.
;
; Returns: CF=0 with [ground_start_lba]/[ground_sectors] set, CF=1 otherwise
;          with a reason on screen.
; ---------------------------------------------------------------------------
find_ground_gpt:
    push eax
    push ebx
    push ecx
    push edx
    push si
    push di
    push es

    mov dword [gpt_found], 0

    ;-- the header ---------------------------------------------------------
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

    ; Keep what the header says before the sum is taken over it: the sum needs
    ; its own field read as zero, and the fields are needed afterwards.
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

    mov edx, 0xFFFFFFFF             ; running sum, pre-inversion
    mov ecx, [gpt_header_bytes]
    mov bx, GPT_OFF_HEADER_CRC
    call crc32_chunk_hole           ; four bytes at BX counted as zero
    not edx
    cmp edx, [gpt_want_crc]
    jne .bad_header_crc

    ;-- the entry array, one window at a time ------------------------------
    mov eax, [gpt_entry_bytes]
    cmp eax, GPT_ENTRY_MIN_BYTES
    jb .bad_array
    cmp eax, GPT_ARRAY_MAX_BYTES
    ja .bad_array
    mov ecx, [gpt_entry_count]
    test ecx, ecx
    jz .bad_array
    mul ecx                         ; EDX:EAX = count * size
    test edx, edx
    jnz .bad_array
    cmp eax, GPT_ARRAY_MAX_BYTES
    ja .bad_array
    mov [gpt_array_bytes], eax

    ; An entry must not straddle two windows, or the walk below would have to
    ; stitch one together. Every table in use has 128-byte entries and a
    ; 4096-byte window holds 32 of them exactly.
    mov eax, GPT_SCRATCH_BYTES
    xor edx, edx
    div dword [gpt_entry_bytes]
    test edx, edx
    jnz .bad_array

    mov eax, [gpt_entry_lba]
    mov [dap_gpt + 8], eax
    mov eax, [gpt_entry_lba + 4]
    mov [dap_gpt + 12], eax

    mov ecx, [gpt_array_bytes]      ; bytes still to sum and walk
    mov edx, 0xFFFFFFFF             ; running sum across every window

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

    ; How much of this window belongs to the array — the last one is short.
    mov eax, ecx
    cmp eax, GPT_SCRATCH_BYTES
    jbe .have_len
    mov eax, GPT_SCRATCH_BYTES
.have_len:
    mov [gpt_window_bytes], eax

    push ecx
    mov ecx, eax
    call crc32_chunk                ; carries EDX across windows
    pop ecx

    call gpt_scan_window            ; remembers a match; does not act on it

    sub ecx, [gpt_window_bytes]

    ; Next window: advance the packet by the sectors just consumed.
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

    ; Only now, with the array proven, is the candidate the answer.
    mov eax, [gpt_cand_first]
    mov [ground_start_lba], eax
    mov eax, [gpt_cand_first + 4]
    mov [ground_start_lba + 4], eax

    ; last - first + 1, and it has to fit what this loader can address.
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

; ---------------------------------------------------------------------------
; gpt_scan_window — look through the entries in the window at ES:0 for one
; whose type is ours, and remember the FIRST such.
;
; Remembering rather than returning: the array this window came out of has not
; been proven yet, and a partition taken from an unverified table is exactly
; what the checksum is for.
;
; Clobbers nothing the caller is carrying (ECX and EDX are the sum and the
; remaining length).
; ---------------------------------------------------------------------------
gpt_scan_window:
    push eax
    push ebx
    push ecx
    push edx
    push si
    push di

    xor si, si                      ; offset of the entry in this window
    mov ecx, [gpt_window_bytes]

.entry:
    cmp ecx, [gpt_entry_bytes]
    jb .done

    cmp dword [gpt_found], 0
    jne .step                       ; already have one; keep summing only

    ; Sixteen bytes of type GUID, four dwords at a time.
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

; ---------------------------------------------------------------------------
; crc32_chunk — CRC-32 (ISO 3309) over ES:0 .. ES:ECX, carrying EDX in and out
; so a sum can span reads. Pre-inverted on entry, NOT inverted on exit: the
; caller inverts once, after the last chunk.
;
; crc32_chunk_hole is the same with the four bytes at BX read as zero, which is
; how a checksum field is summed over itself.
; ---------------------------------------------------------------------------
crc32_chunk:
    mov bx, 0xFFFF                  ; a hole nothing can be at
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
    jb .have                        ; inside the hole: read as zero
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

; ---------------------------------------------------------------------------
; read_deed — fetch the volume's title and satisfy this loader that it is one.
;
; The head sits at the first block of the ground. If it does not check out, the
; copy at the far end does — the last block of the ground, which is where the
; tool that made the volume puts it. That is a real second chance, unlike the
; scan-the-first-64-blocks-for-a-kernel-header fallback it replaces: that one
; found A kernel, not THIS volume's kernel, and could not tell the difference.
;
; Returns: CF=0 with a validated Deed at DEED_ADDR, CF=1 otherwise.
; ---------------------------------------------------------------------------
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

    ; The last whole block of the ground.
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

; ---------------------------------------------------------------------------
; validate_deed — AL = the role this copy is required to claim.
;
; Magic, then the two lengths (bounded BEFORE anything is summed over them),
; then the checksum, then the role. Nothing here is believed until the sum has
; passed, which is why only the fields needed to know how much to sum are read
; first.
;
; Returns: CF=0 if the Deed at DEED_ADDR is one, CF=1 otherwise.
; ---------------------------------------------------------------------------
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

    add cx, ax                      ; prologue + stamps = what the sum covers
    call deed_crc32                 ; ES:0..CX, crc32 field read as zero
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

; ---------------------------------------------------------------------------
; deed_crc32 — CRC-32 (ISO 3309) over ES:0 .. ES:CX, with the four bytes of
; the checksum field itself read as zero, which is how it was written.
;
; Bitwise: a table would be 1 KiB of this loader's remaining window to save
; microseconds on 200 bytes, once, at boot.
;
; Returns: EDX = the sum. Clobbers nothing else.
; ---------------------------------------------------------------------------
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
    jb .have                        ; inside the checksum field: read as zero
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

; ---------------------------------------------------------------------------
; find_kernel_in_deed — walk the stamps for the one that says where the kernel
; is, and take the three numbers out of it.
;
; A stamp names itself, states its own length, and is followed by the next at
; the following four-byte boundary. A reader that meets a stamp it does not
; know steps over it — which is the whole of the format's forward
; compatibility, and is why this loader can be older than the volume.
;
; Returns: CF=0 with kernel_start_block / kernel_block_count / kernel_size_bytes
;          set, CF=1 if this volume names no kernel.
; ---------------------------------------------------------------------------
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

    mov ax, [es:si + 2]             ; payload length
    add ax, 4 + 3
    and ax, 0xFFFC                  ; header + payload, rounded up to four
    cmp cx, ax
    jb .missing                     ; claims more than is left

    cmp word [es:si], VOLUME_STAMP_BOOT
    je .found

    sub cx, ax
    add si, ax
    jmp .walk

.found:
    cmp word [es:si + 2], 12         ; three numbers is the least it can carry
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
    ; Bound the block count by what a kernel can be. Without it a corrupted
    ; Deed drives the load loop into a multi-gigabyte read.
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

; Validate the loaded kernel binary's header magic before handing control to
; long-mode entry. The kernel binary places "KERN" at offset +2 and "EL" at
; offset +6 of its very first sector. If the load is corrupted (a bad read, a
; deed pointing at the wrong blocks), jumping to it would triple-fault
; silently — so halt here with a clear message instead.
;
; Requires: Unreal Mode active (a32 access to 0x100000 needed).
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

    ; Header version at offset +8 must match what the bootloader speaks.
    ; A drift here means the kernel layout (e.g. _kernel_phys_end position)
    ; might have moved — refuse to launch with stale assumptions.
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

; Load kernel file via bounce buffer + Unreal Mode copy to above 1MB.
; Reads sectors to bounce buffer (0x10000) via BIOS INT 13h, then
; restores Unreal Mode (BIOS clobbers DS/ES caches) and copies each
; chunk to KERNEL_RUN_ADDR+ using 32-bit address overrides.
; Requires: enter_unreal_mode called once before load_kernel_tagfs.
; Returns: CF=0 on success (kernel_loaded_bytes set), CF=1 on error
tagfs_load_kernel_file:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    ; The Deed states the kernel's position in the volume's own blocks, so the
    ; sector on the medium is the ground it stands on plus that many blocks.
    ; Nothing here is an absolute address: move the partition, and the same
    ; arithmetic finds the same kernel.
    mov eax, [kernel_start_block]
    shl eax, TAGFS_SECTORS_PER_BLOCK_LOG2   ; block → sector within the volume
    add eax, [ground_start_lba]
    mov edx, [ground_start_lba + 4]
    adc edx, 0
    mov [kernel_load_sector], eax
    mov [kernel_load_sector_hi], edx

    ; total sectors = block_count * sectors_per_block
    mov eax, [kernel_block_count]
    shl eax, TAGFS_SECTORS_PER_BLOCK_LOG2
    mov [kernel_load_sectors], eax

    mov ecx, [kernel_load_sectors]  ; 32-bit counter
    mov ebx, [kernel_load_sector]
    mov edi, KERNEL_RUN_ADDR        ; destination above 1MB (Unreal Mode)

.load_loop:
    ; Sectors to read this pass: min(ecx, 64)
    mov eax, 64
    cmp ecx, 64
    jae .do_read
    mov eax, ecx

.do_read:
    push ecx
    push eax

    ; Set up DAP for this chunk. The sector number is 64 bits: its top half
    ; lives beside the bottom one rather than being assumed zero, because a
    ; volume can sit anywhere a GPT can name and those go past two tebibytes.
    mov [dap_kernel_chunk + 2], ax
    mov word [dap_kernel_chunk + 4], 0x0000
    mov word [dap_kernel_chunk + 6], KERNEL_BOUNCE_SEG
    mov [dap_kernel_chunk + 8], ebx
    push eax
    mov eax, [kernel_load_sector_hi]
    mov [dap_kernel_chunk + 12], eax
    pop eax

    ; No retry loop here. disk_read_dap retries with a controller reset, falls
    ; back from EDD to CHS, and resumes from the sectors that actually landed
    ; instead of re-reading the chunk from its start. A second loop around it
    ; would multiply one budget by another and make the failure that finally
    ; escapes belong to no layer in particular.
    mov si, dap_kernel_chunk
    mov dl, [boot_drive_saved]
    call disk_read_dap
    jc  .read_failed

    pop eax     ; sectors read this pass
    pop ecx     ; remaining sector count

    ; Unreal mode is already back: disk_read_dap restores it on the way out,
    ; for every caller, precisely so that adding a read somewhere cannot
    ; quietly break the copy somewhere else. This is where a second call used
    ; to sit — two mechanisms for one invariant, and the one that gets
    ; maintained is never reliably the same one.
    ;
    ; Copy chunk: bounce buffer (DS:ESI=0x10000) → kernel (ES:EDI=0x100000+)
    ; Both DS and ES now have 4GB limit from restore_unreal_mode.
    push ecx
    movzx ecx, ax
    shl ecx, 7              ; dwords to copy (sectors * 512 / 4)
    mov esi, KERNEL_BOUNCE_ADDR
    cld
    a32 rep movsd           ; 32-bit address override, 4GB-limited DS:ESI and ES:EDI
    pop ecx

    ; Advance LBA and decrement remaining count, carrying into the top half.
    movzx eax, ax
    add ebx, eax
    adc dword [kernel_load_sector_hi], 0
    sub ecx, eax

    ; Sanity: kernel must not exceed KERNEL_MAX_SIZE
    mov eax, edi
    sub eax, KERNEL_RUN_ADDR
    cmp eax, KERNEL_MAX_SIZE
    jae .size_error

    test ecx, ecx
    jnz .load_loop

    ; Record actual loaded byte count for boot_info
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
    ; The disk did nothing wrong here, so do not print its status underneath.
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
    mov edx, 0x534D4150    ; 'SMAP'
    mov ax, E820_SEG
    mov es, ax
    mov di, 4              ; entries start at ES:4 = physical 0x504 (0x500-0x503 = count/size header)
    xor bp, bp

.e820_loop:
    ; Re-establish ES on every iteration. Some real BIOSes (older AMI/Phoenix)
    ; have been known to clobber ES across INT 15h calls; explicit restore
    ; keeps ES:DI pointed at the correct map slot regardless.
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

    ; A zero-length region describes nothing, and firmware does emit them.
    ; Checked before the ACPI test below, not after: a 20-byte entry takes the
    ; short path around that test, and the first version of this check sat on
    ; the far side of it and so never saw one.
    mov eax, [es:di + 8]
    or  eax, [es:di + 12]
    jz .skip_entry

    ; ACPI 3.0 24-byte entries carry a "valid" bit at offset +20 bit 0.
    ; If the BIOS reports 24 bytes AND the valid bit is 0, drop the entry.
    cmp ecx, 24
    jb .accept_entry
    test byte [es:di + 20], 1
    jz .skip_entry

.accept_entry:
    inc bp
    add di, 24

    ; overflow protection: stop if we hit the max entry limit
    cmp bp, E820_MAX_ENTRIES
    jae .e820_done

.skip_entry:
    test ebx, ebx
    jnz .e820_loop

.e820_end:
    ; Two ways in. From above, the firmware said "that was the last one" by
    ; clearing EBX and we walked here. From the carry jump, it said the same
    ; thing by setting the carry flag on a call that was not the first —
    ; firmware is allowed to end the list either way, and Linux's own
    ; detect_memory_e820 breaks out on carry and keeps what it has. The old
    ; code treated that carry as a failure of the whole function: it threw the
    ; collected map away and replaced it with the two or three entries INT 15h
    ; AX=E801 can describe.
    ; A kernel handed that map treats every ACPI table, every firmware
    ; reservation and every MMIO hole below the RAM top as free memory, and
    ; the PMM hands them out. Nothing about that failure looks like a
    ; bootloader bug from where it lands.
    test bp, bp
    jz .e820_fail                   ; the very first call failed: no E820 here

.e820_done:

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

    ; Fallback: create minimal memory map (entries start at ES:4 = physical 0x504)
    mov ax, E820_SEG
    mov es, ax
    mov di, 4              ; entries start at offset 4, matching E820_MAP_ADDR = 0x504

    mov dword [es:di], 0x00000000
    mov dword [es:di+4], 0x00000000
    mov dword [es:di+8], 0x0009FC00    ; 640KB
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1
    mov dword [es:di+20], 0
    add di, 24

    ; Try INT 15h AX=0xE801 — reliable up to 4GB, supported since ~1994.
    ; Returns: AX = KB between 1MB and 16MB (or CX if AX=0)
    ;          BX = 64KB blocks above 16MB   (or DX if BX=0)
    xor cx, cx
    xor dx, dx
    mov ax, 0xE801
    int 0x15
    jc .try_int88

    ; Some BIOSes return results in CX/DX instead of AX/BX
    test ax, ax
    jnz .e801_ax_ok
    mov ax, cx
.e801_ax_ok:
    test bx, bx
    jnz .e801_bx_ok
    mov bx, dx
.e801_bx_ok:

    ; Entry 1: 1MB – (1MB + AX KB)  [0x100000 upward]
    movzx eax, ax
    shl eax, 10                     ; KB → bytes
    mov dword [es:di], 0x00100000
    mov dword [es:di+4], 0x00000000
    mov [es:di+8], eax
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1         ; E820_USABLE
    mov dword [es:di+20], 0
    add di, 24

    ; Entry 2 (optional): above 16MB  [0x1000000 upward, BX * 64KB]
    test bx, bx
    jz .e801_done
    movzx ebx, bx
    shl ebx, 16                     ; 64KB blocks → bytes
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

    ; Last resort: INT 15h 0x88 — caps at 64MB, available on very old BIOSes.
.try_int88:
    mov ah, 0x88
    int 0x15
    jc .memory_fail
    movzx eax, ax
    shl eax, 10                     ; KB → bytes
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

;============================================================================
; disk_read_dap — read sectors per DAP through the firmware (INT 13h).
;
; This replaced a raw ATA PIO reader on 2026-08-23, and the reason is the
; first boot of BoxOS on a real machine. The old code drove ports 0x1F0-0x1F7
; directly, on the stated ground that "the IDE controller itself is rock-solid"
; where BIOS firmware "varies wildly in correctness". Both halves of that are
; true and it is still the wrong trade, because it answers a question nobody
; asked: the controller is only rock-solid if the boot device is ON it. A USB
; stick is not. Neither is an NVMe drive, and neither is a SATA disk on a board
; whose chipset has no IDE compatibility mode — which is every board built in
; the last decade, including the Gigabyte B365 this was found on. The old
; header admitted as much in its own limitations section; what it did not say
; is that "BIOS legacy boot" and "boot from anything that is not IDE" had
; therefore become mutually exclusive.
;
; INT 13h is not a compromise here. It is the only interface that knows how to
; reach the device the firmware itself booted from, whatever that device is,
; and it is the interface the firmware has already used successfully by the
; time we run. The correctness worries are answered where they arise:
;
;   * EDD is probed properly, once per drive — carry clear AND BX=AA55 AND the
;     packet bit in CX. Firmware that does not implement AH=41h may return with
;     carry clear and leave BX alone, and a loader that trusted carry would
;     then call into a handler that is not there.
;   * When EDD is absent, or spends its retry budget refusing, CHS takes over
;     with a budget of its own and resumes where EDD stopped. A CSM shim over
;     a USB stick can answer AH=41h and still fail AH=42h.
;   * Every attempt is retried with a controller reset (AH=00h) between tries,
;     and the budget is renewed after every chunk that lands. A stick behind a
;     CSM commonly refuses its first access of the session and is fine after.
;   * CHS reads one sector per call. AH=02h may not cross a track boundary, and
;     computing where that boundary falls for every geometry the firmware might
;     report is a larger surface than the extra calls cost.
;
; ‼ THE CALLER'S PACKET IS NEVER HANDED TO THE FIRMWARE. AH=42h writes its
; answer into the count field of whatever packet it was given, and every DAP in
; this file is a static structure that gets reused — dap_tagfs_metadata is
; passed to the firmware sixty-four times during a header scan, each time
; expecting to still say "one sector". We read the request out of the caller's
; packet once and drive the firmware with a private one, which is also what
; makes the count field trustworthy enough to check.
;
; ‼ WHAT THE FIRMWARE SAYS IT MOVED IS CHECKED. Both functions report it —
; AH=02h in AL, AH=42h in the packet — and a firmware that transfers less than
; it was asked for returns carry CLEAR. A loader that reads only carry then
; copies whatever was already in the bounce buffer into the kernel image and
; jumps to it. A short read is a failed read; the retry resumes from the
; sectors that did land, so nothing already read is read twice.
;
; The private packet is also the transfer position: the LBA it holds is the
; next sector to fetch and the segment it holds is where that sector goes,
; stepped by 0x20 per sector so the offset never has to wrap. The caller's
; offset is folded into that segment on entry, which is what makes a 64-sector
; request unable to run off the end of a 64 KB window no matter what offset it
; started from.
;
; Unreal mode: INT 13h re-primes DS and ES with a 64 KB real-mode limit, which
; would silently truncate the a32 addressing the kernel copy depends on. The
; reader restores it after every call rather than leaving that to callers, so
; that adding a read somewhere cannot quietly break the copy somewhere else.
;
; Input:  SI = pointer to DAP (EDD layout: size, _, count, off, seg, lba)
;         DL = drive number, exactly as the firmware handed it to stage1
; Output: CF = 0 on success, CF = 1 when every path and every retry failed
;         drd_err = the last BIOS status byte, printed by the failure paths
; Preserves: all GP registers (pushad/popad).
;            DS and ES come back FLAT with base 0 -- unreal mode is restored
;            once, on the way out, and that reload is what their values become.
;            Every caller sets ES for itself immediately after the call, so
;            there is nothing to preserve; saying so beats a push/pop pair that
;            restores a value the restore then overwrites.
; ‼ CF is set AFTER restore_unreal_mode, never before: that routine drives CR0
;   through `or`/`and`, so it destroys flags. Reading CF across it is how the
;   first version of this reader silently never detected EDD.
;============================================================================
DRD_CHUNK   equ 64                  ; sectors per EDD call
DRD_RETRIES equ 5

disk_read_dap:
    pushad

    mov [drd_drive], dl

    ;-- take the request; from here the firmware sees only our own packet ---
    mov ax, [si + 2]
    mov [drd_left], ax
    mov eax, [si + 8]
    mov [drd_pkt + 8], eax
    mov eax, [si + 12]              ; and the top half of the sector number
    mov [drd_pkt + 12], eax

    ; Fold the caller's offset into the segment. Afterwards the offset is
    ; under 16 bytes, so DRD_CHUNK sectors of transfer cannot reach 0x10000
    ; from it — the wrap that would otherwise write the tail of a chunk over
    ; the head of the same buffer.
    mov ax, [si + 4]
    mov bx, ax
    and bx, 0x000F
    mov [drd_pkt + 4], bx
    shr ax, 4
    add ax, [si + 6]
    mov [drd_pkt + 6], ax

    mov byte [drd_pkt + 0], 0x10
    mov byte [drd_pkt + 1], 0
    ; The top half of the LBA is the caller's, not zero. It used to be zeroed
    ; here, which is correct for every sector below two tebibytes and silently
    ; reads the wrong place for any above — and a GPT names a partition in 64
    ; bits precisely because those exist.

    ; Probe once per drive. A second AH=41h per read would be honest and
    ; pointless; a drive does not gain or lose EDD between reads.
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
    test cl, 1                      ; packet access (AH=42h)
    jz .probe_geom
    mov byte [drd_edd], 1

.probe_geom:
    ; Geometry is asked for unconditionally: it is what makes a CHS fallback
    ; expressible, and it costs one call at boot. Heads is stored as DH+1, so
    ; it is never zero when the call succeeded and both fields are zero when
    ; it failed — which is why the loop below tests sectors-per-track alone.
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
    mov byte [drd_err], 0xFF        ; "no BIOS status recorded"
    mov byte [drd_try], DRD_RETRIES

.next:
    cmp word [drd_left], 0
    je .ok

    cmp byte [drd_edd], 1
    je .via_edd

    ;-- CHS, one sector per call -------------------------------------------
    cmp word [drd_spt], 0
    je .exhausted
    ; CHS cannot name a sector above 2 TiB in any encoding. A drive that holds
    ; one answers AH=42h; if it does not, the read is impossible and saying so
    ; beats computing a cylinder out of the bottom half of the address.
    cmp dword [drd_pkt + 12], 0
    jne .exhausted

    ; sector   = (LBA % SPT) + 1
    ; head     = (LBA / SPT) % HEADS
    ; cylinder = (LBA / SPT) / HEADS
    mov eax, [drd_pkt + 8]
    xor edx, edx
    movzx ecx, word [drd_spt]
    div ecx
    inc dl
    mov bl, dl                      ; 1-based sector, held until CL is built
    xor edx, edx
    movzx ecx, word [drd_heads]
    div ecx
    mov dh, dl                      ; head straight into its INT 13h register
    cmp eax, 1023
    ja .exhausted                   ; past what CHS can name

    mov ch, al
    mov cl, ah
    shl cl, 6
    or  cl, bl
    mov dl, [drd_drive]
    mov ax, [drd_pkt + 6]
    mov es, ax
    mov bx, [drd_pkt + 4]
    mov ax, 0x0201                  ; AH=02h read, AL=1 sector
    int 0x13
    jc .retry
    cmp al, 1                       ; AL = sectors actually transferred
    jne .short
    mov ax, 1
    jmp .advance

    ;-- EDD ----------------------------------------------------------------
.via_edd:
    mov ax, [drd_left]
    cmp ax, DRD_CHUNK
    jbe .asking
    mov ax, DRD_CHUNK
.asking:
    mov [drd_asked], ax
    mov [drd_pkt + 2], ax           ; rewritten every call: the firmware's
    mov si, drd_pkt                 ; answer lands in this same field
    mov ah, 0x42
    mov dl, [drd_drive]
    int 0x13
    jc .retry
    mov ax, [drd_pkt + 2]
    test ax, ax
    jz .short                       ; agreed, and moved nothing
    cmp ax, [drd_asked]
    ja .short                       ; claimed more than it was asked for

.advance:                           ; AX = sectors that actually landed
    sub [drd_left], ax
    movzx ecx, ax
    add [drd_pkt + 8], ecx          ; next LBA, carrying into its top half
    adc dword [drd_pkt + 12], 0
    shl cx, 5                       ; sectors * 0x20 = segment step
    add [drd_pkt + 6], cx           ; next destination
    mov byte [drd_try], DRD_RETRIES
    jmp .next

.short:
    ; Carry clear and nothing usable moved. Record that as its own status so
    ; the screen can tell "the firmware refused" from "the firmware agreed".
    xor ah, ah
.retry:
    mov [drd_err], ah
    dec byte [drd_try]
    jz .exhausted
    xor ax, ax                      ; AH=00h — reset, clearing the error latch
    mov dl, [drd_drive]
    int 0x13
    jmp .next

.exhausted:
    ; EDD having failed is not the drive having failed. Hand CHS its own
    ; budget before declaring the read impossible, and stop claiming EDD.
    ; Whatever already landed stays landed — CHS resumes, it does not restart.
    cmp byte [drd_edd], 1
    jne .fail
    mov byte [drd_edd], 0
    mov byte [drd_try], DRD_RETRIES
    jmp .next

.ok:
    call restore_unreal_mode        ; clobbers AX and flags -- both fixed below
    popad
    clc
    ret
.fail:
    call restore_unreal_mode
    popad
    stc
    ret

;--- print_hex8 — AL as two hex digits. The three fatal disk paths print the
;--- BIOS status next to their message: "0x80" is a timeout and the device
;--- never answered, "0x04" is sector-not-found and the geometry is wrong,
;--- "0x00" is a firmware that reported success and moved nothing, and "0xFF"
;--- is no INT 13h having returned a status at all. On a machine with no
;--- serial port that difference is the entire diagnosis.
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
    add al, 7                       ; '9'+1 .. 'A'
.emit:
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    ret

;--- The private packet is the transfer position. drd_pkt+8 is the next sector
;--- to fetch, drd_pkt+6 is where it goes; nothing else holds either number.
align 4
drd_pkt:            times 16 db 0   ; our EDD Disk Address Packet
drd_left:           dw 0            ; sectors still owed to the caller
drd_asked:          dw 0            ; sectors requested of the call in flight
drd_spt:            dw 0
drd_heads:          dw 0
drd_drive:          db 0
drd_probed_for:     db 0xFF         ; 0xFF = nothing probed yet
drd_edd:            db 0
drd_try:            db 0
drd_err:            db 0xFF         ; last BIOS status; 0xFF = never got one
;----------------------------------------------------------------------------
; Everything from here to the end of the file runs in 32-bit protected mode.
;
; This directive is load-bearing and it was lost once, on 2026-08-23, while the
; disk reader above was being rewritten: it had sat between the old PIO
; reader's data and this label, and it went out with the code around it. NASM
; then emitted 16-bit encodings for setup_paging, and the CPU — already in
; protected mode — read the first `call` (E8 64 FF, a rel16) as a rel32 that
; swallowed the two bytes behind it, jumped to 0x51670A3E, and ran off through
; unmapped memory with EIP climbing. Nothing printed, nothing faulted, no
; message said why. The assertion at the bottom of this file exists so that
; deleting this line again is a build error instead of that.
;----------------------------------------------------------------------------
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

    push ecx               ; save total 2MB pages (for fill_pd later)

    ; Zero 32KB of page table space at dynamic address
    mov edi, [dynamic_pt_base]
    mov ecx, 8192          ; 32KB / 4 = 8192 dwords
    xor eax, eax
    rep stosd

    pop ecx                ; ECX = total 2MB pages
    push ecx               ; re-save for fill_pd pop

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

    ; Fill all PD entries with 2MB identity-mapped pages, except the two
    ; 2 MB-aligned guard regions (guard1 between kernel & page tables,
    ; guard2 between page tables & boot stack) — those are left P=0 so a
    ; stack overflow into them takes a #PF instead of silently corrupting
    ; the page tables.
    mov ebp, [guard1_base]
    shr ebp, 21                    ; EBP = guard1 PD index
    mov esi, [guard2_base]
    shr esi, 21                    ; ESI = guard2 PD index

    mov edi, [dynamic_pt_base]
    add edi, 0x2000
    mov eax, 0x000083              ; Present, Writable, Page Size (2MB)
    xor edx, edx                   ; current PD index
    pop ecx                        ; ECX = total 2MB pages

.fill_pd:
    cmp edx, ebp
    je .guard_entry
    cmp edx, esi
    je .guard_entry
    mov [edi], eax
    mov dword [edi+4], 0
    jmp .next_pd
.guard_entry:
    mov dword [edi], 0             ; non-present guard
    mov dword [edi+4], 0
.next_pd:
    add eax, 0x200000
    add edi, 8
    inc edx
    loop .fill_pd

    ; ---- Higher-half kernel mapping ----
    ; Map kernel at 0xFFFFFFFF80000000+ (PML4[511] -> PDPT_high -> PD_high)
    ; PDPT_high at dynamic_pt_base + 0x6000, PD_high at dynamic_pt_base + 0x7000
    ; Both were already zeroed by the rep stosd above.

    ; Copy first 16 PD entries from identity PD[0] to PD_high
    ; This maps first 32MB of physical memory at the higher-half address
    mov esi, [dynamic_pt_base]
    add esi, 0x2000             ; source: identity PD[0]
    mov edi, [dynamic_pt_base]
    add edi, 0x7000             ; dest: PD_high
    mov ecx, 16                 ; 16 entries = 32MB (covers kernel + boot stack)
.copy_pd_high:
    mov eax, [esi]
    mov [edi], eax
    mov eax, [esi+4]
    mov [edi+4], eax
    add esi, 8
    add edi, 8
    dec ecx
    jnz .copy_pd_high

    ; PDPT_high[510] -> PD_high (index 510 = 0xFFFFFFFF80000000 range)
    mov edi, [dynamic_pt_base]
    add edi, 0x6000             ; PDPT_high base
    mov eax, [dynamic_pt_base]
    add eax, 0x7000
    or eax, 3                   ; Present + Writable
    mov [edi + 510*8], eax
    mov dword [edi + 510*8 + 4], 0

    ; PML4[511] -> PDPT_high
    mov edi, [dynamic_pt_base]  ; PML4 base
    mov eax, [dynamic_pt_base]
    add eax, 0x6000
    or eax, 3                   ; Present + Writable
    mov [edi + 511*8], eax
    mov dword [edi + 511*8 + 4], 0

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

; Sector 0 of the medium — its partition table, which is what says where this
; volume's ground is. A fixed LBA, and the only one left in this loader: it is
; where every partition table on every medium has always been.
align 4
dap_sector0:
    db 0x10, 0
    dw 1
    dw 0x0000
    dw MBR_SEG
    dq 0

; The Deed. Its LBA is filled in at run time from the partition table — first
; the head of the ground, then, if that will not check out, the far end.
; The GPT: header first, then the entry array a window at a time. Its LBA and
; length are both filled in at run time.
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
    dw 64               ; updated dynamically
    dw 0x0000           ; updated dynamically
    dw 0x1000           ; updated dynamically
    dq 0                ; updated dynamically

align 4
boot_drive_saved:       db 0

; Twelve bytes, NUL-padded, copied verbatim onto the boarding pass so the
; kernel can say which loader produced the image it is running.
boarding_loader_name:   db 'stage2', 0, 0, 0, 0, 0, 0

; Drive-select base byte for raw ATA PIO LBA28. Computed once on first call
; from boot_drive_saved (BIOS DL = 0x80 master, 0x81 slave). Bits encoded:
;   0xE0 = 1110_xxxx = master + LBA mode
;   0xF0 = 1111_xxxx = slave  + LBA mode
; The low nibble carries LBA[27:24] at issue time and is OR-ed in per sector.
align 4
dap_work_lba:           dd 0
dap_work_count:         dd 0
dap_work_seg:           dw 0
dap_work_off:           dw 0

align 4
dynamic_pt_base:        dd 0            ; computed page table base (after kernel)
dynamic_stack_base:     dd 0            ; computed stack base (after page tables)
guard1_base:            dd 0            ; 2 MB guard between kernel and page tables
guard2_base:            dd 0            ; 2 MB guard between page tables and boot stack

kernel_file_id:         dw 0
kernel_start_block:     dd 0
kernel_block_count:     dd 0
; The ground, as this loader holds it: a 64-bit start, because a GPT names a
; partition in 64 bits and disks that need them exist, and a 32-bit length,
; because a BoxOS volume longer than two tebibytes is refused out loud below
; rather than truncated in silence.
align 4
ground_start_lba:       dq 0            ; first sector of the volume's ground
ground_sectors:         dd 0            ; how far it runs
deed_want_role:         db 0            ; which copy validate_deed is checking

; What the GPT header said, held while its own checksum is still being taken.
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

; The BoxOS partition type, cf8ae49a-d26a-4959-9a6f-71c932e0c9eb, in the
; mixed-endian order GPT stores a type GUID in — which is the order it appears
; on the medium, and therefore the order to compare in. The same sixteen bytes
; the kernel's ground.c and the UEFI loader carry; permanent, never reissued.
BOXOS_TYPE_GUID_0       equ 0xcf8ae49a
BOXOS_TYPE_GUID_1       equ 0x4959d26a
BOXOS_TYPE_GUID_2       equ 0xc9716f9a
BOXOS_TYPE_GUID_3       equ 0xebc9e032
kernel_load_sector:     dd 0
kernel_load_sector_hi:  dd 0
kernel_load_sectors:    dd 0
kernel_size_bytes:      dd 0            ; actual file size from TagFS metadata
kernel_loaded_bytes:    dd 0            ; total bytes loaded (sectors * 512)

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
; (page table validation now done dynamically in 32-bit mode via VGA "NO MEM" on failure)
msg_kernel_bad_magic      db '[FATAL] Kernel header magic mismatch — corrupted load!', 13, 10, 0
msg_kernel_bad_version    db '[FATAL] Kernel header version mismatch — rebuild required!', 13, 10, 0

;=============================================================================
; Assemble-time invariants. These cost nothing at runtime and each one stands
; for a failure that has actually happened or is one edit away.
;=============================================================================

; The tail of this file must be assembled as 32-bit code. Stated here, far from
; the [BITS 32] it checks, precisely so that deleting that directive together
; with the code around it — which is how it was lost — cannot pass the build.
%if __?BITS?__ != 32
  %error "stage2 must end in BITS 32: the protected-mode block lost its directive"
%endif

; The image must fit the window stage1 loads it into, and everything the
; loader places after that window must actually be after it. Until 2026-08-23
; neither held: boot_info was written 0x4F bytes inside this binary, and
; nothing in the build would have said a word if the binary had grown past
; sixteen sectors — `dd seek=1` would simply have written the seventeenth over
; the first sector of the kernel, and stage1 would have loaded a truncated
; stage2 that verified its own signature happily.
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

; The other half of a claim stage1 makes: its scratch sits at 0x1200 because
; the largest map this loader can write ends below that. If E820_MAX_ENTRIES
; ever grows, the map reaches into a region stage1 documents as its own.
%if E820_MAP_ADDR + E820_MAX_ENTRIES * 24 > 0x1200
  %error "the E820 map has grown into stage1's scratch at 0x1200"
%endif
