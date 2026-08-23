;=============================================================================
; BoxOS stage1 — the 512 bytes the firmware hands control to.
;
; Rewritten 2026-08-23, after the first boot on real hardware refused with
; "LBA required!" on a Gigabyte B365 M AORUS ELITE booting a USB stick. The
; probe that produced the rewrite printed this:
;
;     DL=00 CF=01 AX=4AEF BX=0000 CX=003F
;
; and every field of it is a lesson:
;
;   * DL printed 00 although the drive check three instructions earlier had
;     already accepted it as 0x80 or 0x81 — so the byte was written, the BIOS
;     was called, and the byte was gone. The old code kept its state INSIDE
;     the boot sector, which firmware is free to reuse. QEMU never touched it;
;     this board did. State now lives outside the sector entirely.
;
;   * AX/BX/CX are not an EDD reply at all. AH=41h answers with AH=version and
;     BX=AA55; those values are neither. They are what was left in the
;     registers, which is the same story told twice.
;
;   * The old code demanded DL be 0x80 or 0x81 and refused everything else.
;     But 0x00 is what USB-FDD emulation legitimately hands a bootloader, and
;     the firmware is not guessing when it does so — it is telling us which
;     handle to use. Second-guessing the BIOS about its own device numbering
;     is how a bootloader becomes unbootable. We take whatever we are given
;     and hand the same value to stage2.
;
; What this loader promises, in order of preference and with no cliff between
; them: read through EDD (INT 13h AH=42h) when the firmware really has it, and
; through CHS (AH=02h) when it does not — one sector at a time on both paths,
; verifying what the firmware says it moved, retrying with a controller reset,
; and renewing the retry budget after every sector that lands.
;
; When it cannot, it says so in three characters rather than dying quietly:
; a letter for the stage that failed and the BIOS status byte in hex. On a
; machine with no serial port and no debugger that trail is the whole of the
; evidence, and this bring-up has already spent one session without it.
;
; Layout it depends on — the same map stage2 declares, and the two must agree:
;   0x00500..0x01103  E820 memory map, written later by stage2
;   0x01200..0x01217  scratch (ours; drive number, EDD flag, geometry, DAP)
;   0x07000           stage2's 16-bit stack top, grows down
;   0x07C00           this sector, as loaded by the firmware
;   0x08000..0x09FFF  stage2, 16 sectors from LBA 1
;=============================================================================
[BITS 16]
[ORG 0x7C00]

;--- Scratch at 0x1200. It began at 0x0500 — right, in that firmware does not
;--- own that page, and wrong, in that stage2 puts the E820 map at exactly
;--- 0x0500 and zeroes the region before filling it. Nothing breaks today,
;--- because stage2 copies the drive number out of DL on its first
;--- instruction and our scratch is already dead when the map lands on it.
;--- But two components claiming one address, each documenting it as its own,
;--- is a bug that has not gone off yet rather than the absence of one: the
;--- day stage2 wants the geometry we probed, it would read a memory map.
;--- 0x1200 sits above the largest map E820 can produce (0x504 + 128*24 =
;--- 0x1104), far below the stack, and is claimed by nobody.
SCRATCH         equ 0x1200
S_DRIVE         equ SCRATCH + 0x00      ; byte  — drive number as the BIOS gave it
S_EDD           equ SCRATCH + 0x01      ; byte  — 1 = EDD usable on this drive
S_SPT           equ SCRATCH + 0x02      ; word  — sectors per track (CHS)
S_HEADS         equ SCRATCH + 0x04      ; word  — heads (CHS)
S_DAP           equ SCRATCH + 0x08      ; 16 bytes — EDD Disk Address Packet
SCRATCH_WORDS   equ 12                  ; 24 bytes, covering everything above

STAGE2_SEG      equ 0x0000
STAGE2_OFF      equ 0x8000
STAGE2_LBA      equ 1
; How many sectors of stage2 to load. The image build owns this number — it
; decides where stage2 is written and where the kernel begins — and passes it
; in with -D, so the loader and the layout cannot drift apart. No default:
; a default is a second place the number is written down.
%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must come from the build (-DSTAGE2_SECTORS=...)"
%endif
STAGE2_SIG      equ 0x2907
RETRIES         equ 5

start:
    ; The firmware may hand us anything in the segment registers. Establish a
    ; known world before touching memory: flat data, and a stack that grows
    ; down from just below this sector, where nothing of ours lives.
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    ; Wipe scratch before writing it. Zero is the answer every field here
    ; means when it is absent — no EDD, no geometry — so the failure paths
    ; below need no code of their own to say "we did not find out".
    mov di, SCRATCH
    mov cx, SCRATCH_WORDS
    rep stosw

    ; DL is the only thing the firmware tells us and the only thing we cannot
    ; recompute. Save it first, before any call that might disturb memory, and
    ; save it OUTSIDE this sector.
    mov [S_DRIVE], dl

    mov al, 'B'                     ; sign of life: the firmware reached us
    call putc

    ;-- Does this drive really do EDD? ------------------------------------
    ; Three conditions, not one. CF clear alone is not enough: a firmware that
    ; does not implement the function may return with carry clear and leave
    ; BX untouched, and a loader that trusted CF would then issue AH=42h into
    ; a handler that does not exist. The magic must come back byte-swapped and
    ; the packet-access bit must be set.
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [S_DRIVE]
    int 0x13
    jc .no_edd
    cmp bx, 0xAA55
    jne .no_edd
    test cl, 1                      ; CX bit 0 — packet access (AH=42h/43h)
    jz .no_edd
    mov byte [S_EDD], 1
.no_edd:

    ;-- Geometry, for the CHS path and as a sanity check for both ---------
    ; Asked unconditionally: it costs one call and it is the only way to know
    ; whether a CHS fallback is even expressible. ES:DI is zeroed first
    ; because some firmware walks it. On failure the zeroed scratch already
    ; says "no geometry" and read_sectors reads it that way.
    push es
    xor ax, ax
    mov es, ax
    xor di, di
    mov ah, 0x08
    mov dl, [S_DRIVE]
    int 0x13
    pop es
    jc .no_geom
    ; CL bits 5:0 = sectors per track; CL bits 7:6 + CH = last cylinder
    ; DH = last head number (so heads = DH + 1)
    movzx ax, cl
    and ax, 0x3F
    mov [S_SPT], ax
    movzx ax, dh
    inc ax
    mov [S_HEADS], ax
.no_geom:

    ;-- Load stage2 -------------------------------------------------------
    mov eax, STAGE2_LBA
    mov cx, STAGE2_SECTORS
    mov bx, STAGE2_OFF
    mov dx, STAGE2_SEG
    call read_sectors
    mov al, 'D'
    jc die_status

    ; Stage2 layout: byte 0 = `jmp short past_sig` (EB 02), bytes 2-3 = the
    ; signature word, byte 4+ = real code. We verify at +2 and enter at +0;
    ; the built-in jump steps over the signature so the CPU never executes
    ; data as instructions.
    mov ax, [STAGE2_OFF + 2]
    cmp ax, STAGE2_SIG
    mov al, 'S'
    jne die_char

    ; Hand stage2 the drive number the firmware gave us — read from scratch,
    ; not from a register, because every INT 13h above was entitled to clobber
    ; DL and several firmwares do.
    mov dl, [S_DRIVE]
    jmp STAGE2_SEG:STAGE2_OFF

;=============================================================================
; read_sectors — read CX sectors from LBA EAX into DX:BX.
;
; One sector per call, on both paths, and the count the firmware says it moved
; is checked against the one we asked for. Three reasons, none theoretical:
;
;   * AH=02h may not cross a track boundary. The old code handed the firmware
;     all sixteen sectors in one call and trusted the geometry to be generous.
;     On a stick the firmware presents as a 1.44 MB floppy — which is what USB
;     media without a partition table commonly becomes — a track is 18 sectors,
;     and a request starting at sector 2 and running sixteen long fits only by
;     luck. One sector per call cannot straddle anything.
;
;   * Both read functions report how much they actually transferred: AH=02h in
;     AL, AH=42h in the packet's count field. Firmware is allowed to return
;     carry clear and a smaller number, and a loader that reads only the carry
;     flag then treats whatever was already in the buffer as data it loaded.
;     We compare, and a short read is a failed read.
;
;   * The retry budget is renewed after every sector that lands. A stick that
;     hiccups once every few hundred sectors would otherwise spend one shared
;     budget and give up mid-transfer; a sector that is genuinely unreadable
;     still exhausts its own budget and stops the boot.
;
; A retry resumes; it does not restart. Sectors already in memory stay there,
; which is also what lets the EDD→CHS fallback pick up mid-transfer.
;
; ‼ The Disk Address Packet in scratch IS the transfer state — there is no
; second copy of the LBA or the destination anywhere. The EDD path hands the
; firmware the very words the loop is stepping; the CHS path reads the same
; words and converts them. Two representations of one position is how a reader
; drifts from itself, and this is a boot sector: there is no room to hold the
; same number twice, and no reason to.
;
;   +0  packet size (0x10)     +6  destination segment — steps 0x20/sector
;   +2  block count (1)        +8  LBA low 32          — steps 1/sector
;   +4  destination offset     +12 LBA high 32 — zero, and stays zero
;
; The destination advances by segment, not offset, so it cannot wrap out of a
; 64 KB window no matter how long the transfer runs: 0x20 of segment is 512
; bytes of address, and the offset never changes at all.
;
; The count field is rewritten before every call because the firmware writes
; its answer into it, and a firmware that answers 0 would otherwise leave the
; next request asking for nothing.
;
; Input:  EAX = starting LBA, CX = sector count, DX:BX = destination seg:off
; Output: CF = 0 on success, CF = 1 when every attempt failed
;         rs_err = last BIOS status byte, or 0xFF if no call ever returned one
; Clobbers: nothing the caller needs (all saved)
;=============================================================================
read_sectors:
    pushad                          ; NOT pusha: the LBA maths below is 32-bit
    push es

    mov [S_DAP + 8], eax            ; LBA — and the loop's position in it
    mov [rs_left], cx
    mov [S_DAP + 4], bx             ; destination offset, fixed for the transfer
    mov [S_DAP + 6], dx             ; destination segment, stepped per sector
    mov byte [S_DAP + 0], 0x10      ; +1 and +12..15 are already zero: scratch
    mov byte [rs_try], RETRIES      ; was wiped before any of this ran

.next:
    cmp word [rs_left], 0
    je .done

    cmp byte [S_EDD], 1
    je .via_edd

    ;-- CHS ---------------------------------------------------------------
    ; LBA → CHS needs geometry, and one test covers it: heads is written as
    ; DH+1, so it is never zero when the geometry call succeeded, and both
    ; fields are zero when it did not. Testing sectors-per-track alone is
    ; therefore exactly as safe as testing both, and this is a boot sector.
    cmp word [S_SPT], 0
    je .exhausted

    ; sector   = (LBA % SPT) + 1
    ; head     = (LBA / SPT) % HEADS
    ; cylinder = (LBA / SPT) / HEADS
    mov eax, [S_DAP + 8]
    xor edx, edx
    movzx ecx, word [S_SPT]
    div ecx                         ; EAX = LBA/SPT, EDX = LBA%SPT
    inc dl
    mov bl, dl                      ; 1-based sector, held until CL is built
    xor edx, edx
    movzx ecx, word [S_HEADS]
    div ecx                         ; EAX = cylinder, EDX = head
    mov dh, dl                      ; head into its INT 13h register at once
    cmp eax, 1023                   ; CHS cannot address past cylinder 1023
    ja .exhausted

    mov ch, al                      ; cylinder low 8 bits
    mov cl, ah
    shl cl, 6                       ; cylinder high 2 bits → CL[7:6]
    or  cl, bl                      ; sector into CL[5:0]
    mov dl, [S_DRIVE]
    mov ax, [S_DAP + 6]
    mov es, ax
    mov bx, [S_DAP + 4]
    mov ax, 0x0201                  ; AH=02h read, AL=1 sector
    int 0x13
    jc .retry
    cmp al, 1                       ; AL = sectors actually transferred
    jne .short
    jmp .advance

    ;-- EDD ---------------------------------------------------------------
.via_edd:
    mov word [S_DAP + 2], 1         ; the firmware overwrites this with its answer
    mov si, S_DAP
    mov ah, 0x42
    mov dl, [S_DRIVE]
    int 0x13
    jc .retry
    cmp word [S_DAP + 2], 1         ; and this is that answer, read back
    jne .short

.advance:
    ; One sector landed. Step the position the firmware reads next time and
    ; give the following sector a full budget of its own.
    inc dword [S_DAP + 8]
    add word [S_DAP + 6], 0x20      ; 0x20 of segment = 512 bytes of address
    dec word [rs_left]
    mov byte [rs_try], RETRIES
    jmp .next

.short:
    ; Carry clear, nothing moved. Record it as its own status so the screen
    ; can tell "the firmware refused" from "the firmware agreed and lied".
    xor ah, ah
.retry:
    mov [rs_err], ah                ; BIOS status, for the failure message
    dec byte [rs_try]
    jz .exhausted
    xor ax, ax                      ; AH=00h — reset the controller
    mov dl, [S_DRIVE]
    int 0x13
    jmp .next

.done:
    pop es
    popad
    clc
    ret

.exhausted:
    ; EDD just spent its whole budget. A drive can answer AH=41h and still
    ; refuse AH=42h — CSM shims translating for a USB stick are the usual
    ; case — and the old way may work where the new one does not. Give CHS
    ; its own budget once, then stop pretending EDD is there.
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

;=============================================================================
; The whole of stage1's voice. Three characters is what fits and, as it turns
; out, what is needed: which stage gave up, and what the firmware said when it
; did. "D80" is a timeout — the device never answered. "D04" is sector not
; found — the geometry is wrong. "DFF" is neither: no INT 13h ever returned a
; status, so CHS was impossible and EDD was absent. "S" is a stage2 that
; loaded and is not stage2.
;=============================================================================
putc:                               ; AL = character. Clobbers BX — every
    mov ah, 0x0E                    ; caller either sets BX afterwards or is
    xor bx, bx                      ; on its way to a halt.
    int 0x10
    ret

puthex:                             ; AL = byte, printed as two hex digits
    push ax
    shr al, 4
    call .nibble                    ; .nibble tail-jumps to putc, whose ret
    pop ax                          ; lands here — no second call needed
    and al, 0x0F
.nibble:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 7                       ; '9'+1 .. 'A'
    jmp putc

die_status:                         ; AL = stage letter, then the BIOS status
    call putc
    mov al, [rs_err]
    call puthex
    jmp hang
die_char:                           ; AL = stage letter alone
    call putc
hang:
    cli
.spin:
    hlt
    jmp .spin

;--- read_sectors working storage. These live in the sector because they are
;--- only touched between our own instructions, never across a BIOS call —
;--- the packet the firmware reads is in scratch for exactly that reason.
rs_left         dw 0                ; sectors still owed to the caller
rs_try          db 0                ; attempts left on the sector in flight
rs_err          db 0xFF             ; last BIOS status; 0xFF = never got one

;=============================================================================
; Bytes 446..509 are the MBR partition table. Nothing of ours may live there:
; firmware and every partitioning tool on earth read those 64 bytes as four
; 16-byte partition records whatever we put in them, and an image without a
; table is one the firmware has to guess about — which is how a 24 MB stick
; becomes an emulated 1.44 MB floppy whose sectors run out under the kernel.
; The space is reserved and asserted now; the image build fills it.
;=============================================================================
%if ($ - $$) > 446
  %error "stage1 exceeds 446 bytes — the MBR partition table has no room"
%endif

times 446-($-$$) db 0
times 64 db 0                       ; partition table — empty until the build fills it
dw 0xAA55
