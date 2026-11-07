# Inside BoxASM

How the assembler and linker are built, where each part lives, and how they
are held to their witnesses. For whoever changes them.

## Where it lives

BoxASM is a program, not part of the kernel: the `assemble` utility, built
like `files` or `tag` and run inside BoxOS on its volume. Its sources are
`src/userspace/utils/assemble/`; the same sources, all but the command's own
files, are built on the host as `tools/boxasm_stand.c`'s stand.

```text
src/userspace/utils/assemble/   the assembler, the linker and the command
tools/boxasm_stand.sh           the boxasm gate: every check, held to NASM and GNU ld
tools/boxasm_stand.c            the host driver: assemble, link, a volume from a directory
tools/boxasm_oracle.py          the generated corpus, the macro program, the float witness,
                                and the comparison of two ELF objects
src/userspace/*.asm, *.inc      the examples the image carries (tags code, asm, example)
src/userspace/try               the Itinerary `try`: assemble, then run
docs/boxasm/                    this documentation
```

## From source to program

```text
source text
  │  boxasm_macro*.c     preprocessor: %define, %macro, %if, %rep, contexts,
  │                      %include and incbin, found on the volume (boxasm_locate.c)
  ▼
expanded text + where each line came from (BoxAsmExpansion)
  │  boxasm_lex.c        tokens
  │  boxasm_parse.c      statements: labels, data, instructions, directives
  │  boxasm_operand.c    registers, memory operands, sizes
  │  boxasm_expr.c       expressions and symbols
  │  boxasm_struc.c, boxasm_form.c, boxasm_box.c
  ▼
statements
  │  boxasm_layout.c     passes until every jump has its size and every label
  │                      its place; alignment proven; bytes emitted
  │  boxasm_isa*.c       the instruction set: a form chosen, its bytes written
  ▼
sections, relocations, symbols
  │  boxasm_object.c, boxasm_elf.c     an ELF64 relocatable object
  ▼
object
  │  boxasm_link*.c      linked with BoxLib's start code and libbox.a, both
  │                      carried inside `assemble` itself; .nameplate, .boxos.form,
  │                      .boxos.recipe added
  ▼
program (ELF executable)  →  written to the volume by assemble_output.c
```

`BoxAsmSourceAssemble` (`boxasm_source.c`) runs the first half and returns
the object; `BoxAsmLink` (`boxasm_link.c`) the second. Everything happens in
memory: a source that does not assemble or link leaves no file behind.

## The files

| File | What it holds |
|---|---|
| `assemble.c`, `assemble.h` | the command: its Form, the request, the Recipe followed, the build |
| `assemble_output.c` | the program written, the one it replaces found, the questions asked |
| `assemble_volume.c` | the volume as a `BoxAsmFiles`; every file read is kept for the Recipe |
| `boxasm_source.c`, `.h` | the pipeline's entry, the stack budget, the files interface, tags |
| `boxasm_locate.c` | which file a name and tags mean, and the refusal that says what is there |
| `boxasm_macro.c` | the preprocessor's lines, macro calls, directives, emission |
| `boxasm_macro_text.c` | tokens, parameter substitution, expansion of `%define`, `%if` expressions |
| `boxasm_macro_define.c` | the macro tables: `%define` and `%macro`, case-blind ones too |
| `boxasm_macro_if.c` | `%if` and its kin |
| `boxasm_macro_include.c` | `%include` and `incbin` |
| `boxasm_macro_context.c` | `%push`, `%pop`, `%repl`, `%$name` |
| `boxasm_macro_string.c` | `%strlen`, `%substr`, `%strcat` |
| `boxasm_lex.c`, `.h` | the lexer |
| `boxasm_parse.c` | statements and directives |
| `boxasm_operand.c` | operands and addresses |
| `boxasm_expr.c` | expressions, symbols, labels |
| `boxasm_struc.c` | `struc` and `istruc` |
| `boxasm_form.c` | `takes` and `gives` → `.boxos.form` |
| `boxasm_box.c` | `box.say`, `box.exit` |
| `boxasm_float.c`, `.h` | decimal constants to binary32/64, exactly |
| `boxasm_isa.h` | operand classes, form flags, the form and instruction types |
| `boxasm_isa.c` | the form an instruction's operands match, and why none does |
| `boxasm_isa_emit.c` | prefixes, REX or VEX, opcode, ModRM, SIB, displacement, immediates |
| `boxasm_isa_table.c` | general instructions, SSE to SSE4.2, MMX forms; the mnemonic lookup |
| `boxasm_isa_vex.c` | AVX, AVX2, FMA, F16C, BMI1, BMI2 |
| `boxasm_isa_x87.c` | x87, fxsave, fxrstor |
| `boxasm_layout.c` | the layout passes and the bytes |
| `boxasm_object.c`, `boxasm_elf.c`, `.h` | the ELF object |
| `boxasm_link.c`, `_names.c`, `_symbols.c`, `_output.c`, `_internal.h` | the linker |
| `boxasm_support.c`, `.h` | buffers, messages, the name index |
| `boxasm_asm.h` | the assembler's state shared by its parts |

## The instruction tables

An instruction is a name and its forms, tried in order (`BoxAsmForm`,
`boxasm_isa.h`). A form says, for each operand, its class — where the operand
goes and what it may be: `REG` (ModRM.reg), `RM` (ModRM.rm, register or
memory), `XMM_V` (VEX.vvvv), `FPU` (added to the opcode), `SIZED_M32` (memory
said to be a dword), and so on — then the operation sizes it takes, the
opcode bytes, the ModRM digit, its flags (VEX map, L and W; WAIT; the operand
left out that is the first again) and its mandatory prefix.

The order of forms is NASM's: when several match, the first is the one NASM
writes. That is how a register pair, an immediate that fits a byte, or a
memory operand without a size comes to the same bytes.

Adding an instruction:

1. Write its forms in the table of its family, and its name in the table's
   list (`PLAIN`, `ENTRY`, `X87`…).
2. If its operands are of a kind no class says, add a class
   (`boxasm_isa.h`), what it holds (`BoxAsmClassHolds`), and where it goes
   (`boxasm_isa_emit.c`).
3. Add its forms to the corpus generator in `tools/boxasm_oracle.py`: every
   register in every place, memory sized and not, immediates at the edges.
4. Run `tools/boxasm_stand.sh`. The corpus must equal NASM's object byte for
   byte; a refusal worth keeping goes into the stand as a check.

## How it is held to NASM

- **The corpus.** `tools/boxasm_oracle.py corpus` writes every supported
  instruction form, with registers 0 to 15 of every width, every address
  form, immediates at the edges of their fields, every condition code, jumps
  both ways; `corpus nasm` writes the same in NASM's spelling. Both are
  assembled, and `boxasm_oracle.py compare` holds the two objects equal:
  the size and bytes of `.text`, `.rodata`, `.data` and `.bss`, and their
  relocations reduced to what the linker makes of them.
- **The macro program.** `boxasm_oracle.py macros` is one program using every
  preprocessor feature both assemblers share; its object must equal NASM's.
- **Floats.** `boxasm_oracle.py floats` writes floating-point constants, ties
  among them, and the bytes their exact rounding gives.
- **The linker** is held to GNU ld with `src/userspace/boxlib/user.ld` on
  the very inputs a BoxOS image links, and its name table to
  `tools/nameplate`.
- **Refusals.** 173 checks that a wrong source — or a wrong program to link,
  or a file asked for that is not one — is refused with the words it must be
  refused with.
- **Accept and refuse.** When a set of instructions is added, every shape of
  0 to 3 operands from a pool of operand kinds (registers of each width,
  vector and x87 registers, memory of each size and none, immediates) goes
  through both assemblers line by line: BoxASM must take nothing NASM
  refuses, and what NASM alone takes is listed in [nasm.md](nasm.md).

`tools/boxasm_stand.sh` runs all of it on the host; it is the `boxasm` gate
of `tools/gates.sh`. A wave of instructions is also run end to end in BoxOS:
a program written there with them, assembled and started with `try`, its
output compared with the host's.

## Limits

- The expanded source is at most 16 MiB, and the preprocessor walks at most
  50 million lines (a `%rep` that runs away).
- How deep a source nests — macro calls, `%define`s, files, parentheses,
  names defined by names — is measured on the stack, which grows to 8 MiB;
  `assemble` keeps 24 KiB of it for itself.
- A section holds at most 4 GiB; the bytes of all strings and `incbin`
  files together at most 4 GiB.
- A Form holds at most 16 fields.
