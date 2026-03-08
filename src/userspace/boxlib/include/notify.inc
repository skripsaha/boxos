; notify.inc — BoxOS PocketRing macros for NASM assembly.
; Mirrors the C API from box/notify.h (Pocket + PocketRing architecture).
;
; PocketRing layout (packed struct at POCKET_RING, 4KB page):
;   0x00  head          uint32_t  (kernel advances)
;   0x04  tail          uint32_t  (userspace advances)
;   0x08  slots[42]     Pocket[42] (42 * 96 = 4032 bytes)
;
; Pocket layout (96 bytes):
;   0x00  pid           uint32_t  (kernel overwrites)
;   0x04  target_pid    uint32_t
;   0x08  error_code    uint32_t
;   0x0C  prefix_count  uint8_t
;   0x0D  current_prefix_idx uint8_t
;   0x0E  flags         uint8_t
;   0x0F  _reserved     uint8_t
;   0x10  data_length   uint32_t
;   0x14  data_addr     uint64_t
;   0x1C  route_tag[32] char[32]
;   0x3C  prefixes[16]  uint16_t[16]
;   0x5C  _pad[4]       uint8_t[4]
;   total = 96 bytes

%ifndef NOTIFY_INC
%define NOTIFY_INC

; -------------------------------------------------------------------------
; Constants
; -------------------------------------------------------------------------

POCKET_RING        equ 0x2000
POCKET_RING_HEAD   equ 0x00
POCKET_RING_TAIL   equ 0x04
POCKET_RING_SLOTS  equ 0x08

POCKET_SIZE        equ 96
POCKET_RING_CAP    equ 42

; Pocket field offsets
PKT_PID            equ 0x00
PKT_TARGET_PID     equ 0x04
PKT_ERROR_CODE     equ 0x08
PKT_PREFIX_COUNT   equ 0x0C
PKT_CURRENT_IDX    equ 0x0D
PKT_FLAGS          equ 0x0E
PKT_DATA_LENGTH    equ 0x10
PKT_DATA_ADDR      equ 0x14
PKT_ROUTE_TAG      equ 0x1C
PKT_PREFIXES       equ 0x3C

POCKET_FLAG_YIELD  equ 0x80

DECK_EXECUTION     equ 0x00
DECK_OPERATIONS    equ 0x01
DECK_STORAGE       equ 0x02
DECK_HARDWARE      equ 0x03
DECK_SYSTEM        equ 0xFF

; -------------------------------------------------------------------------
; pocket_push — push a Pocket to the PocketRing from stack-local buffer.
; Input: rdi = pointer to 96-byte Pocket struct
; Output: eax = 0 on success, -1 if ring full
; Clobbers: rax, rcx, rdx, rsi
; -------------------------------------------------------------------------
%macro pocket_push 0
    mov eax, POCKET_RING
    mov ecx, [rax + POCKET_RING_TAIL]
    ; Check if full: (tail + 1) % CAP == head
    lea edx, [ecx + 1]
    push rbx
    mov ebx, POCKET_RING_CAP
    xor edx, edx
    mov eax, ecx
    inc eax
    div ebx                     ; edx = (tail+1) % CAP
    mov eax, POCKET_RING
    cmp edx, [rax + POCKET_RING_HEAD]
    pop rbx
    je %%full
    ; Copy 96 bytes from rdi to ring->slots[tail]
    ; slot_addr = POCKET_RING + POCKET_RING_SLOTS + tail * POCKET_SIZE
    imul ecx, ecx, POCKET_SIZE
    lea rsi, [rax + POCKET_RING_SLOTS]
    add rsi, rcx
    ; rdi = source, rsi = dest — swap for rep movsb
    push rdi
    push rsi
    pop rdi          ; dest = slot
    pop rsi          ; source = pocket
    mov ecx, POCKET_SIZE
    rep movsb
    ; Advance tail
    mov eax, POCKET_RING
    mov ecx, [rax + POCKET_RING_TAIL]
    inc ecx
    push rbx
    mov ebx, POCKET_RING_CAP
    xor edx, edx
    mov eax, ecx
    div ebx
    mov eax, POCKET_RING
    mov [rax + POCKET_RING_TAIL], edx
    pop rbx
    xor eax, eax     ; return 0 = success
    jmp %%done
%%full:
    mov eax, -1
%%done:
%endmacro

; -------------------------------------------------------------------------
; YIELD — send a yield hint to the scheduler.
; Pushes an empty Pocket with POCKET_FLAG_YIELD, fires notify.
; Uses stack-allocated 96-byte buffer.
; Clobbers: rax, rcx, rdx, rdi, rsi, r11
; -------------------------------------------------------------------------
%macro YIELD 0
    sub rsp, 96
    mov rdi, rsp
    ; Zero the pocket
    push rcx
    mov ecx, 96
%%zero:
    mov byte [rdi + rcx - 1], 0
    dec ecx
    jnz %%zero
    pop rcx
    ; Set yield flag
    mov byte [rsp + PKT_FLAGS], POCKET_FLAG_YIELD
    ; Push to ring
    mov rdi, rsp
    pocket_push
    add rsp, 96
    syscall                             ; notify
%endmacro

%endif ; NOTIFY_INC
