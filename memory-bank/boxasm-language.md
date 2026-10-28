# BoxASM — the assembler inside BoxOS

Status 2026-10-28: implemented in `src/userspace/utils/assemble/`, held to
NASM 3 byte for byte and to GNU ld by `tools/boxasm_stand.sh` (the `boxasm`
gate), run end to end in BoxOS (`hello`, `countdown`).

## Using it

```text
use code asm example
assemble countdown.asm                       -> countdown.elf
assemble countdown.asm -> count.elf (tags test)
countdown
```

The result wears the source's tags plus the ones the command names. When it
would wear neither `app` nor `utility`, `assemble` adds `app` and says so,
because the shell starts nothing else. A program that does not assemble or
link leaves no file behind. Errors read `file:line:column: what is wrong`,
followed by the line and a caret under the place.

## Source

- One statement per line; `;` starts a comment. Mnemonics, registers and
  directives are case-insensitive; names are case-sensitive.
- `name:` defines a label. Before `db dw dd dq resb resw resd resq times equ`
  the colon may be left out: `msg db "Hi", 10, 0`.
- `.name` is local to the last plain label: `main` + `.loop` is `main.loop`.
- `section .text | .rodata | .data | .bss`, `global a, b`, `extern printf`,
  `default rel | abs`, `bits 64`.

### Data

- `db dw dd dq` take numbers, names and strings. A string in `dw dd dq` is
  padded with zeros to the item width. `dd` and `dq` may hold an address.
- `resb resw resd resq N` reserve space, only in `.bss`. Elsewhere use
  `times N db 0`.
- `times N` repeats one instruction or one `db/dw/dd/dq`.
- `align N` (a power of two up to 4096): pads code with `nop` and data with
  zeros.
- `name equ expression`. `$` is the address of the statement, `$$` the start
  of its section: `len equ $ - msg`.
- Strings are written `"..."` or `'...'` and take the escapes `\n \t \r \0 \\
  \" \' \xHH`. Used as a number, a string of up to 8 bytes is read
  little-endian: `mov al, 'A'`.

### Expressions

`| ^ & << >> + - * / %`, unary `- ~ +`, parentheses, with NASM's precedence.
They are computed as signed 64-bit integers, the way C computes them. An
address takes only `+` and `-` of a number. Two addresses in one section
subtract to a number.

### Operands

- Registers: `rax..r15`, `eax..r15d`, `ax..r15w`, `al..r15b` (also `r8l`),
  `spl bpl sil dil`, and `ah bh ch dh` (which no instruction needing REX may
  use).
- Memory: `[base + index*scale + displacement]`. The scale (1, 2, 4, 8) may
  be written first: `[8*rcx + table]`.
- `[label]` is RIP-relative. `[abs label]` is the 32-bit absolute address,
  and `[rip + label]` is the same as `[rel label]`. An address made of a bare
  number is absolute.
- `byte word dword qword` say the size of a memory operand where no register
  does: `inc qword [rax]`. Instructions with one size need none:
  `push [rax]`, `jmp [rbx]`, `sete [rcx]`.
- `short` and `near` before a jump target force rel8 or rel32. Without them,
  the assembler takes the shortest form that reaches.

### Instructions

`mov movzx movsx movsxd lea xchg push pop add or adc sbb and sub xor cmp test
inc dec not neg mul imul div idiv rol ror rcl rcr shl sal shr sar jmp call ret
jcc setcc cmovcc` (with every condition spelling), and `nop int3 leave cqo cdq
cwd cdqe cwde cbw pushfq popfq cld std endbr64 pause ud2`. Each encodes as
NASM encodes it.

### box operations

- `box.say "text"` or `box.say label` (NUL-ended) prints through BoxLib's
  `print`, the screen Current.
- `box.exit code` or `box.exit register` ends the program through BoxLib's
  `exit` with the low 32 bits of the code.

A box operation keeps every general register and RFLAGS. Vector registers
follow the C call ABI and may change. Box operations push onto the stack:
BoxOS userspace has no red zone.

## The program contract

- `main` is a label in `.text`, declared `global`. `_start` calls it as a C
  function: rsp is 8 below a 16-byte boundary on entry. Returning from main
  ends the program with eax as its code.
- A call to an outside function (`extern printf`) follows the System V call
  ABI: rsp aligned to 16 at the call, rbx rbp r12–r15 kept, and al set to the
  number of vector registers a variadic callee receives.
- CET: on a CET machine BoxOS turns on indirect-branch tracking and the
  shadow stack for user programs. Every address of code the program uses as a
  value (`lea`, `mov reg, label`, `push label`, `dq label`) must point at
  `endbr64`, and the assembler refuses one that does not. `ret` returns only
  to where a `call` came from.

## Where BoxASM differs from NASM

- Strings in `"..."` take C escapes. NASM gives escapes only to backquoted
  strings.
- `/ % >>` are signed, as in C. In NASM they are unsigned (`//` and `%%` are
  its signed forms).
- `near` keeps a jump rel32. NASM keeps it only with `strict near`.
- `align` in a data section pads with zeros. NASM pads with `nop` unless
  told `db 0`.
- `[label]` is RIP-relative without `default rel`.
- `[rip + x]` is accepted as a spelling of `[rel x]`.

Everything else in the shared subset assembles to the same bytes and
relocations: `tools/boxasm_oracle.py` generates a corpus of about 5,000
lines and compares the two objects.

## Not built yet

- Artifact Resolution, as agreed: updating the exact `name + tags` match
  instead of creating a second file.
- SSE/AVX instructions; raw Manifest, `box.notify`, `box.wait`.
- Programs made of several sources; a native BoxOS object format.
