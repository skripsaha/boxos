# BoxASM

BoxASM is the assembler of BoxOS, and it runs inside BoxOS: a person writes a
program on the machine, assembles it there, and starts it by name. No other
computer, no cross-compiler.

```nasm
section .text
global main

main:
    box.say "Hello from BoxASM!\n"
    box.exit 0
```

```text
use code asm example
assemble hello.asm
hello
```

It reads NASM's language and writes, for every instruction form NASM and it
share, the very bytes NASM 3 writes — held to that on a generated corpus of
about 36,000 lines and on millions of operand shapes. It links the program
with BoxLib, the C library of BoxOS, so a program in assembly calls the same
functions a program in C does: `printf`, files and tags, memory, Touch,
strands. The program it writes remembers what it was made from, and is made
again when its source changes.

## What it has

- **Instructions**: all of x86-64-v3 a program runs — the general ones, x87,
  MMX, SSE to SSE4.2, AVX, AVX2, FMA, F16C, BMI1, BMI2, MOVBE.
- **Data**: bytes to qwords, strings with escapes, floating-point constants
  rounded exactly, `times`, `align`, `struc`, `incbin`.
- **The preprocessor** of NASM: `%define`, `%macro`, `%if`, `%rep`, contexts,
  string functions, `%include`.
- **Files found the BoxOS way**: by name and tags, never by a path.
- **Errors in words**: the line, a caret under the place, the macro calls it
  came through, and what to write instead.
- **A linker** held to GNU ld, and programs with a Form the shell fills, a
  Recipe, and a name table for faults.

## The chapters

| Chapter | For |
|---|---|
| [tutorial.md](tutorial.md) | writing programs, from the first line to files and Touch |
| [language.md](language.md) | the language: lines, names, data, expressions, operands, the program contract |
| [instructions.md](instructions.md) | every instruction, by family, with what it needs of the processor |
| [preprocessor.md](preprocessor.md) | macros, conditions, contexts, `%include`, `incbin` |
| [boxlib.md](boxlib.md) | calling BoxLib from assembly: the ABI and the functions |
| [assemble.md](assemble.md) | the `assemble` command: names, tags, errors, the Recipe, `try` |
| [nasm.md](nasm.md) | where BoxASM and NASM part, and porting a NASM source |
| [internals.md](internals.md) | how it is built and tested, for whoever changes it |

## Examples on the volume

The image carries sources tagged `code, asm, example`: `hello.asm`,
`countdown.asm` (a loop, a function, `printf`), `machine.asm` (`cpuid`, a
locked counter), `fractions.asm` (SSE and floating-point output),
`macros.asm` (the preprocessor), `planets.asm` with `planets.inc`, and the
library `lines.inc` (tag `asmlib`) it reaches with `%include … everywhere`.
`use code asm example`, then `files` lists them; `show NAME` prints one.

## Not built

- AVX-512, and the instructions only the kernel may run.
- `box.notify` and `box.wait` as box operations, and raw Manifests: a program
  reaches Touch and every system operation through BoxLib's functions.
- One program from several sources assembled apart; a source takes others in
  with `%include`.
- Of NASM's preprocessor, `%defstr %deftok`, `%[...]`, `%line`, `%pragma`,
  `%depend` and `%pathsearch`.

## Where it lives

The assembler, the linker and the `assemble` command are one program,
`src/userspace/utils/assemble/`. It is userspace, as it should be: it is a
tool a person runs, like `files` or `tag`, and it needs nothing of the kernel
another program does not. Its stand and oracle are in `tools/`; see
[internals.md](internals.md).
