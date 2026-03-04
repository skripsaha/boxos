; notify.inc — BoxOS notify page macros for NASM assembly.
; Mirrors the C API from box/notify.h and box/chain.h.
;
; Notify page layout (packed struct at NOTIFY_PAGE):
;   0x00  magic         uint32_t
;   0x04  prefix_count  uint8_t
;   0x05  flags         uint8_t
;   0x06  status        uint8_t
;   0x07  reserved1     uint8_t
;   0x08  prefixes[16]  uint16_t each  (32 bytes total)
;   0x28  data[256]     uint8_t

%ifndef NOTIFY_INC
%define NOTIFY_INC

; -------------------------------------------------------------------------
; Constants
; -------------------------------------------------------------------------

NOTIFY_PAGE        equ 0x1000

NP_MAGIC           equ 0x00
NP_PREFIX_COUNT    equ 0x04
NP_FLAGS           equ 0x05
NP_STATUS          equ 0x06
NP_PREFIXES        equ 0x08
NP_DATA            equ 0x28

NOTIFY_MAGIC       equ 0x4E4F5449   ; "NOTI"

NOTIFY_FLAG_YIELD  equ 0x80

DECK_EXECUTION     equ 0x00
DECK_OPERATIONS    equ 0x01
DECK_STORAGE       equ 0x02
DECK_HARDWARE      equ 0x03
DECK_SYSTEM        equ 0xFF

; -------------------------------------------------------------------------
; notify_prepare — initialize notify page for a new syscall.
; Sets magic, clears prefix_count, flags, status.
; Clobbers: rax
; -------------------------------------------------------------------------
%macro notify_prepare 0
    mov eax, NOTIFY_PAGE
    mov dword [rax + NP_MAGIC],        NOTIFY_MAGIC
    mov byte  [rax + NP_PREFIX_COUNT], 0
    mov byte  [rax + NP_FLAGS],        0
    mov byte  [rax + NP_STATUS],       0
%endmacro

; -------------------------------------------------------------------------
; notify_prefix deck_id, opcode — append one prefix to the page.
; Prefix encoding: (deck_id << 8) | opcode, stored as uint16_t.
; Clobbers: rax, ecx
; -------------------------------------------------------------------------
%macro notify_prefix 2
    mov eax, NOTIFY_PAGE
    movzx ecx, byte [rax + NP_PREFIX_COUNT]
    ; ecx zero-extends to rcx; rax zero-extends to 64-bit base — use rax, rcx
    mov word [rax + rcx * 2 + NP_PREFIXES], ((%1) << 8) | (%2)
    inc byte [rax + NP_PREFIX_COUNT]
%endmacro

; -------------------------------------------------------------------------
; notify_send — issue int 0x80, then clear magic.
; Returns event_id in rax.
; Uses rbx internally (push/pop) to preserve rax across magic clear.
; -------------------------------------------------------------------------
%macro notify_send 0
    int 0x80
    push rbx
    mov ebx, NOTIFY_PAGE
    mov dword [rbx + NP_MAGIC], 0
    pop rbx
%endmacro

; -------------------------------------------------------------------------
; NOTIFY deck0, op0 [, deck1, op1 ...] — all-in-one variadic syscall macro.
; Prepares page, adds all prefix pairs, fires int 0x80, clears magic.
; Arguments must come in (deck_id, opcode) pairs.
; Returns event_id in rax.
; Uses rbx for cleanup (push/pop).
; -------------------------------------------------------------------------
%macro NOTIFY 2-*
    notify_prepare
%rep (%0 / 2)
    notify_prefix %1, %2
    %rotate 2
%endrep
    int 0x80
    push rbx
    mov ebx, NOTIFY_PAGE
    mov dword [rbx + NP_MAGIC], 0
    pop rbx
%endmacro

; -------------------------------------------------------------------------
; YIELD — send a yield hint to the scheduler.
; Sets NOTIFY_FLAG_YIELD, fires int 0x80, clears magic.
; Clobbers: rax
; -------------------------------------------------------------------------
%macro YIELD 0
    mov eax, NOTIFY_PAGE
    mov dword [rax + NP_MAGIC],        NOTIFY_MAGIC
    mov byte  [rax + NP_PREFIX_COUNT], 0
    mov byte  [rax + NP_FLAGS],        NOTIFY_FLAG_YIELD
    mov byte  [rax + NP_STATUS],       0
    int 0x80
    mov eax, NOTIFY_PAGE
    mov dword [rax + NP_MAGIC], 0
%endmacro

%endif ; NOTIFY_INC
