# The BoxASM language

This is the specification of the source BoxASM reads: what a line is, what
names, data, expressions and operands are, the structures and directives, and
the contract a program keeps with BoxOS. The instructions are in
[instructions.md](instructions.md), the preprocessor in
[preprocessor.md](preprocessor.md).

BoxASM speaks NASM's language. A source NASM assembles in 64-bit mode
assembles here to the same bytes, but for the places listed in
[nasm.md](nasm.md).

## Lines

- One statement per line. `;` starts a comment that runs to the end of the
  line.
- A statement is `[label[:]] [instruction or directive] [operands]`. A line
  may hold only a label, only a comment, or nothing.
- Mnemonics, register names, size words and directives are read in any case:
  `MOV`, `mov` and `Mov` are one. Names — labels, constants, macros — are
  case-sensitive.
- A prefix (`lock`, `rep`, `repe`, `repne`) shares its instruction's line.

## Names

A name starts with a letter, `_` or `.` and continues with letters, digits,
`_`, `.` and `@`. Register names, mnemonics and directives are not names.

- `name:` defines a label: the address of the next statement. Before `db dw
  dd dq resb resw resd resq times incbin equ` the colon may be left out: `msg
  db "Hi", 10, 0`.
- A label beginning with `.` is local to the last plain label before it:
  after `main:`, `.loop:` is `main.loop`, and may be reached from anywhere by
  that full name.
- `..@` names belong to the preprocessor (`%%label` of a macro call, `%$name`
  of a context) and do not change which plain label `.local` names attach to.
- `name equ expression` makes a constant. Its expression is evaluated once,
  and a constant may be used before the line that defines it.
- A name used and never defined is refused, unless it is declared
  `extern`; then the linker finds it in BoxLib or another object.

## Sections

```text
section .text      code; read and execute
section .rodata    read-only data
section .data      data that may change
section .bss       zeroed space, reserved with resb resw resd resq
```

`segment` is a spelling of `section`. A source starts in `.text`. Any other
section name is refused. `default rel` and `default abs` set what a bare
`[label]` means (BoxASM's default is `rel`); `bits 64` is accepted and is the
only width there is.

`global a, b` makes labels visible to the linker; `main` must be global.
`extern printf, malloc` declares names defined outside the source.

## Data

- `db dw dd dq` lay out bytes, words, dwords and qwords. Each item is a
  number, a name, an expression or a string. A string in `dw dd dq` is padded
  with zeros to the item's width. `dd` and `dq` may hold an address (`dq
  table`), which the linker fills in.
- `resb resw resd resq N` reserve N items of zeros, only in `.bss`. Outside
  it, write `times N db 0`.
- `times N statement` repeats one instruction, one `db/dw/dd/dq`, or one
  `incbin`, N from 0 to 2^32 − 1.
- `align N` (N a power of two, 1 to 4096) pads to a multiple of N: with `nop`
  in code, with zeros in data. Inside a `struc`, `alignb` is its spelling too.
- `incbin "name"` lays out the bytes of a file; see
  [preprocessor.md](preprocessor.md#including-a-files-bytes-incbin).
- `$` is the address of the statement it stands in, `$$` the start of its
  section: `len equ $ - msg`.

### Strings and characters

Strings are written `"..."` or `'...'`. Both take the escapes `\n \t \r \0
\\ \" \' \xHH`. Used where a number is meant, a string of up to 8 bytes is
read little-endian: `mov al, 'A'`, `mov eax, 'abcd'`.

### Floating-point constants

`dd` takes 32-bit and `dq` 64-bit floating-point constants:

```nasm
dq 3.14159, -2.5, 1.e10, 6.022e23
dd 0.1, __Infinity__, -__QNaN__
```

The `.` is what makes a constant floating: `dd 1` is the integer 1. `_` may
stand among the digits. `__Infinity__`, `__QNaN__` (or `__NaN__`) and
`__SNaN__` are the special values. Each constant is rounded once, straight
from its decimal digits to the nearest value of its width, ties to even. One
too large for its width, or not zero but rounding to zero, is refused.

Instructions take no floating-point immediates: a constant lives in data and
is loaded from there (`movsd xmm0, [pi]`).

## Expressions

| Operators, lowest first | |
|---|---|
| `\|` | or |
| `^` | exclusive or |
| `&` | and |
| `<< >>` | shifts |
| `+ -` | sum |
| `* / %` | product |
| unary `- ~ +`, parentheses | |

Expressions are computed as signed 64-bit integers, the way C computes them:
`/`, `%` and `>>` are signed. An address takes only `+` and `-` of a number;
two addresses in one section subtract to a number.

## Operands

- **Registers**: `rax..r15`, `eax..r15d`, `ax..r15w`, `al..r15b` (also
  `r8l..r15l`), `spl bpl sil dil`, and `ah bh ch dh` (which no instruction
  needing a REX prefix may use); `xmm0..xmm15`, `ymm0..ymm15`, `mm0..mm7`,
  and `st0..st7` for the x87 stack.
- **Memory**: `[base + index*scale + displacement]`, the base and index
  64-bit registers, the scale 1, 2, 4 or 8, which may be written first
  (`[8*rcx + table]`). A gather's index is an xmm or ymm register
  (`[rax + ymm1*4]`); no other address takes one.
- `[label]` is RIP-relative. `[abs label]` is the 32-bit absolute address.
  `[rel label]` and `[rip + label]` are RIP-relative written out. An address
  made only of a number is absolute.
- **Sizes**: `byte word dword qword tword oword yword` (1, 2, 4, 8, 10, 16,
  32 bytes) say the size of a memory operand where no register does: `inc
  qword [rax]`. An instruction of one size needs none: `push [rax]`, `jmp
  [rbx]`, `sete [rcx]`. A memory operand without a size where the
  instruction has several is refused, never guessed.
- **Immediates**: any expression. A field one byte wide takes -128 to 255; a
  16-bit field -32768 to 65535; a 32-bit field its 32-bit range; a 64-bit
  operation sign-extends a 32-bit immediate, but `mov r64, imm` takes all 64
  bits.
- **Branch targets**: `short` and `near` before a jump target force the rel8
  or rel32 form; without them the shortest that reaches is taken.

## Prefixes

`lock` stands before an instruction the processor lets lock (`add adc and btc
btr bts cmpxchg cmpxchg8b cmpxchg16b dec inc neg not or sbb sub xadd xchg
xor`) whose destination is memory. `rep` stands before `movs stos lods`;
`repe`/`repz` and `repne`/`repnz` before `cmps scas`, written without
operands (`rep movsb`). Any other pairing is refused: the processor would
reject it, or read it as another instruction (`rep bsf` is `tzcnt`).

## Structures

`struc NAME` … `endstruc` writes a layout. Its labels (`.x` is `NAME.x`) are
offsets from 0, made by `resb resw resd resq` and `align` (or `alignb`), and
`NAME_size` is where it ends. They are constants:

```nasm
struc point
    .x: resd 1
    .y: resd 1
endstruc

    mov eax, [rbx + point.y]
    add rbx, point_size
```

`istruc NAME` … `iend` lays one instance out in data. `at point.y, dd 2`
fills with zeros up to the field and writes it there; `iend` fills to
`NAME_size`:

```nasm
origin:
    istruc point
        at point.x, dd 10
        at point.y, dd 20
    iend
```

## The Form: what a program takes and gives

A program tells the shell what it takes, so the shell sorts the words a
person typed into the program's fields, in their order, and asks only when it
must (the C side is `box/form.h`):

```text
takes thing "the file to tag"            one file that is here
takes optional things "the files"        a list of files, may be none
takes words "the tags they get"          as many words as are typed
takes name "the new name"                one word, new or not
takes text "the note"                    the rest of the line as typed
takes into "where the result goes"       the name `->` points at
gives things                             what the program gives back:
gives text                               files, or text (one of the two)
```

- `takes` and `gives` stand at the start of a statement, outside `struc` and
  `istruc`, in any section. The fields keep the order they are written in.
- The kind is one of `thing`, `things`, `name`, `words`, `text`, `into`, and
  `optional` may stand before it. The words in quotes (a few words, none
  empty, with no newline, NUL or double quote) are what the shell says when
  it asks for the field.
- A Form holds at most 16 fields, `into` once, `gives` once.
- The assembler writes the Form as the section `.boxos.form`: the line `form
  1`, then one line per directive, as the C macro `BOX_FORM` does. The linker
  puts it read-only after `.nameplate`, where the shell finds it, and defines
  `__boxos_form_start` and `__boxos_form_end` around it.

What the person typed reaches the program as its Luggage and filled Form;
the program reads them with BoxLib's `form_things`, `form_words` and
`luggage_word` (see [boxlib.md](boxlib.md)).

## box operations

Two operations need no `extern`:

- `box.say "text"` or `box.say label` (a NUL-ended string) prints through
  BoxLib's `print`, to the screen Current.
- `box.exit code` or `box.exit register` ends the program through BoxLib's
  `exit` with the low 32 bits of the code.

A box operation keeps every general register and RFLAGS. Vector registers
follow the C call ABI and may change. Box operations push onto the stack.

Everything else BoxLib offers — printing with formats, files and tags,
memory, Touch, strands, time — is called as C functions; see
[boxlib.md](boxlib.md).

## The program contract

- **Entry.** `main` is a label in `.text`, declared `global`. BoxLib's
  `_start` calls it as a C function, with no arguments: rsp is 8 below a
  16-byte boundary when `main` begins. Returning from `main` ends the
  program with eax as its code; so does `box.exit`, or BoxLib's `exit`.
- **Arguments.** A program gets no argc/argv. What the person typed is its
  Luggage and its filled Form, read through BoxLib.
- **Calls.** A call to an outside function follows the System V AMD64 call
  ABI: integer arguments in rdi, rsi, rdx, rcx, r8, r9, then on the stack;
  floating ones in xmm0..xmm7; rsp a multiple of 16 at the `call`; rbx, rbp
  and r12–r15 kept by the callee; every other register may change; al holds
  how many vector registers a variadic callee (`printf`) receives. The value
  comes back in rax, or xmm0.
- **No red zone.** BoxOS userspace keeps none: BoxLib is built without one
  and box operations push below rsp, so nothing below rsp is the program's
  to keep.
- **The stack** grows on demand to 8 MiB.
- **CET.** On a processor with CET, BoxOS turns on indirect-branch tracking
  and the shadow stack for user programs. Every address of code the program
  uses as a value (`lea`, `mov reg, label`, `push label`, `dq label`) must
  point at `endbr64`, and the assembler refuses one that does not. `ret`
  returns only to where a `call` came from.
- **Alignment.** Packed SSE memory operands (and AVX's aligned moves) must
  sit on their boundary; for a label the assembler proves it or refuses the
  line (see [instructions.md](instructions.md)).
- **The processor.** The assembler cannot know the machine a program will
  run on. An instruction of a set the processor lacks stops the program:
  ask `cpuid` first (see [instructions.md](instructions.md#processor-features)).
