# Writing programs in BoxASM

This is a walk from a first line to a program that reads files, waits for
keys and runs two strands at once. The programs of sections 1 and 3 to 7
were typed into BoxOS, assembled there and run, and what they printed is
shown under them; every piece of source here assembles as written.

The pages it leans on: [language.md](language.md) for the language,
[boxlib.md](boxlib.md) for the functions a program calls,
[assemble.md](assemble.md) for the command.

## 1. A first program

Write the source as a file of the volume — in `draft`, BoxOS's editor, with
`say … -> hello.asm` a line at a time, or carried in on the image. Then:

```nasm
section .text
global main

main:
    box.say "Hello from BoxASM!\n"
    box.exit 0
```

```text
assemble hello.asm
hello
Hello from BoxASM!
```

`main` is where the program starts; it is `global` so the linker finds it.
`box.say` prints a string, `box.exit` ends the program with its code. `try
hello.asm` does both steps at once.

## 2. Data, a loop, a function

```nasm
section .rodata
line db "  %d", 10, 0

section .text
extern printf
global main

main:
    push rbx                ; kept for main's caller; rsp is now a multiple of 16
    mov ebx, 5
.next:
    lea rdi, [line]
    mov esi, ebx
    xor eax, eax            ; printf is variadic: al counts vector registers
    call printf
    dec ebx
    jnz .next
    pop rbx
    xor eax, eax
    ret
```

Data lives in `.rodata` (read-only) or `.data`; `.next` is a label local to
`main`. `printf` is BoxLib's, declared `extern` and called as C calls it:
the first arguments in rdi, rsi, rdx, rcx, r8, r9, rsp a multiple of 16 at
the `call`, rbx and r12–r15 kept across it. `countdown.asm` among the
examples goes on with a function of its own.

## 3. What the person typed: the Form

A program says what it takes, and the shell sorts the words typed after its
name into those fields, asking for what is missing. The program reads them
with `form_words` or `form_things`.

```nasm
; words.asm — the words typed after the program's name, one a line
takes optional words "the words to show"

section .rodata
line: db "%lu: %s", 10, 0

section .bss
words: resq 1
count: resq 1

section .text
extern form_words, printf
global main

main:
    push rbx
    xor edi, edi
    lea rsi, [words]
    lea rdx, [count]
    call form_words
    test eax, eax
    jnz .done
    xor ebx, ebx
.next:
    cmp rbx, [count]
    jae .done
    mov rax, [words]
    mov rdx, [rax + rbx*8]
    lea rsi, [rbx + 1]
    lea rdi, [line]
    xor eax, eax
    call printf
    inc rbx
    jmp .next
.done:
    pop rbx
    xor eax, eax
    ret
```

```text
assemble words.asm
words alpha beta "two words"
1: alpha
2: beta
3: two words
```

`form_words(0, &words, &count)` gives the words of field 0 as an array of
strings; the program walks it.

## 4. A file: size and tags

`takes thing` asks for one file. The shell finds it by name in the Use
Context, and asks which when there are several.

```nasm
; measure.asm — a file's size and tags
takes thing "the file to measure"

section .rodata
said: db "%s: %lu bytes, tags %s", 10, 0
none: db "(none)", 0

section .bss
ids:   resq 1
count: resq 1
meta:  resb 48                  ; FileMetadata

section .text
extern form_things, FileMetadataRead, FileMetadataRelease, printf
global main

main:
    sub rsp, 8
    xor edi, edi
    lea rsi, [ids]
    lea rdx, [count]
    call form_things
    test eax, eax
    jnz .failed
    mov rax, [ids]
    mov rdi, [rax]              ; the one file's id
    lea rsi, [meta]
    call FileMetadataRead
    test eax, eax
    jnz .failed
    mov rsi, [meta + 32]        ; Filename
    mov rdx, [meta + 16]        ; Size
    mov rcx, [meta + 40]        ; Tags, or NULL
    lea rax, [none]
    test rcx, rcx
    cmovz rcx, rax
    lea rdi, [said]
    xor eax, eax
    call printf
    lea rdi, [meta]
    call FileMetadataRelease
    xor eax, eax
    add rsp, 8
    ret
.failed:
    neg eax                     ; the error code, as the program's exit code
    add rsp, 8
    ret
```

```text
use code asm example
assemble measure.asm
measure hello.asm
hello.asm: 83 bytes, tags hello,code,asm,example
```

`FileMetadataRead` fills a `FileMetadata`; its fields are read at their
offsets (`boxlib.md` gives the layout). When something fails, the program
ends with the error's code.

## 5. Writing a file

```nasm
; note.asm — writes a short note into a new file
takes name "the new file's name"

section .rodata
text:   db "BoxASM wrote this.", 10
length  equ $ - text
tags:   db "note", 0
wrote:  db "wrote %ld bytes to %s", 10, 0
failed: db "note: error %d", 10, 0

section .bss
words: resq 1
count: resq 1
file:  resq 1

section .text
extern form_words, create, file_span, fwrite, printf
global main

main:
    push rbx
    xor edi, edi
    lea rsi, [words]
    lea rdx, [count]
    call form_words
    test eax, eax
    jnz .error
    mov rax, [words]
    mov rbx, [rax]              ; the name
    mov rdi, rbx
    lea rsi, [tags]
    call create                 ; the new file's id, or -error
    test rax, rax
    js .error
    mov [file], rax
    mov rdi, rax
    mov esi, length
    xor edx, edx
    call file_span              ; the file made `length` bytes long
    test eax, eax
    jnz .error
    mov rdi, [file]
    xor esi, esi                ; at offset 0
    lea rdx, [text]
    mov ecx, length
    call fwrite                 ; bytes written, or -error
    test rax, rax
    js .error
    lea rdi, [wrote]
    mov rsi, rax
    mov rdx, rbx
    xor eax, eax
    call printf
    xor eax, eax
    pop rbx
    ret
.error:
    neg eax
    mov esi, eax
    lea rdi, [failed]
    xor eax, eax
    call printf
    mov eax, 1
    pop rbx
    ret
```

```text
assemble note.asm
note memo.txt
wrote 19 bytes to memo.txt
measure memo.txt
memo.txt: 19 bytes, tags memo,note,code,asm,example
```

`create` makes the file in the Use Context, wearing the context's tags and
its own; `file_span` gives it its size, and `fwrite` writes within it.

## 6. Waiting for keys: Touch

A program does not poll: it waits for an event, and the machine sleeps
while it waits. `console_listen` names the tag this program's keys arrive
on; `touch_wait_tag` sleeps until one does.

```nasm
; keys.asm — names each key pressed, until q
section .rodata
hint: db "press keys; q ends", 10, 0
key:  db "key '%c' (scancode %u)", 10, 0

section .bss
tag:   resd 1
align 8
touch: resb 120                 ; one Touch

section .text
extern console_listen, touch_wait_tag, printf
global main

main:
    sub rsp, 8
    lea rdi, [hint]
    xor eax, eax
    call printf
    call console_listen         ; the tag this strand's keys arrive on
    mov [tag], eax
.wait:
    mov edi, [tag]
    lea rsi, [touch]
    xor edx, edx                ; no timeout
    call touch_wait_tag
    test al, al
    jz .wait
    movzx edx, byte [touch + 24]    ; kb_event_t: scancode,
    movzx esi, byte [touch + 25]    ; ascii, mods
    cmp esi, 'q'
    je .done
    test esi, esi
    jz .wait                    ; a key with no character: a release, an arrow
    lea rdi, [key]
    xor eax, eax
    call printf
    jmp .wait
.done:
    xor eax, eax
    add rsp, 8
    ret
```

```text
try keys.asm
press keys; q ends
key 'b' (scancode 48)
key 'o' (scancode 24)
key 'x' (scancode 45)
```

The event is a `Touch`: its payload, at offset 24, is the key — scancode,
character, modifiers.

## 7. Two strands

A strand runs beside `main` in the same memory. The function it runs begins
with `endbr64`, because its address is handed to BoxLib and reached by an
indirect call. `main` sleeps on a word with `addr_park` until the strand
changes it and wakes it with `addr_wake`.

```nasm
; strand.asm — a second strand sums 1..1000 while main waits for it
section .rodata
said: db "the strand summed %lu", 10, 0

section .bss
align 8
done:   resq 1
result: resq 1

section .text
extern strand_spawn, addr_park, addr_wake, printf
global main

; the strand: rdi = n; returning ends it
worker:
    endbr64                     ; its address is handed to strand_spawn
    sub rsp, 8
    xor eax, eax
.add:
    add rax, rdi
    dec rdi
    jnz .add
    mov [result], rax
    mov qword [done], 1
    lea rdi, [done]
    xor esi, esi                ; wake every waiter
    call addr_wake
    add rsp, 8
    ret

main:
    sub rsp, 8
    lea rdi, [worker]
    mov esi, 1000
    call strand_spawn           ; the new strand's pid, 0 when none
    test eax, eax
    jz .failed
.wait:
    cmp qword [done], 0
    jne .ready
    lea rdi, [done]
    xor esi, esi                ; sleep while done is still 0
    xor edx, edx                ; no timeout
    call addr_park
    jmp .wait
.ready:
    lea rdi, [said]
    mov rsi, [result]
    xor eax, eax
    call printf
    xor eax, eax
    add rsp, 8
    ret
.failed:
    mov eax, 1
    add rsp, 8
    ret
```

```text
try strand.asm
the strand summed 500500
```

## 8. Macros and a library

The preprocessor writes what repeats. `planets.asm` among the examples takes
in a library kept apart from it:

```nasm
%include "lines.inc" asmlib everywhere
%include "planets.inc"
```

`lines.inc` wears `asmlib`, and `everywhere` reaches past the Use Context to
it; it defines a macro `line` that formats and prints. See
[preprocessor.md](preprocessor.md).

## 9. Floating point and vectors

`fractions.asm` computes with SSE and prints with `%f`; the instructions of
x87, SSE, AVX, AVX2 and FMA are listed in [instructions.md](instructions.md).
A floating-point constant lives in data (`pi dq 3.14159`) and is loaded from
there; a `double` goes to `printf` in xmm0 with `al` = 1.

## 10. When it goes wrong

- **The assembler refuses a line**: the message says the file, line and
  column, shows the line, and very often what to write instead.
- **A name is never defined**: a BoxLib function needs its `extern`.
- **The program stops at a call**: rsp was not a multiple of 16 at the
  `call`, or a variadic call had a stale `al`.
- **An address of code taken as a value** must begin with `endbr64`.
- **A packed SSE operation stops the program**: its memory is not on a
  16-byte boundary; `align 16` before the data.
- **An instruction the processor lacks** stops the program: ask `cpuid` (see
  `machine.asm`). Under QEMU, start BoxOS with `STRICT=on` for SSSE3 and
  later.
