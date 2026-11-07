# BoxASM and NASM

BoxASM reads NASM's language, and NASM 3 is its witness: for every form both
take, the two write the same bytes and relocations
([internals.md](internals.md#how-it-is-held-to-nasm)). Where they part, it is
listed here. Most of the parting is BoxASM refusing what NASM takes and
would get wrong at run time, or takes silently and means something the
writer did not.

## Porting a NASM source

1. Start the file with `default rel` if it relies on NASM's default (BoxASM's
   `[label]` is RIP-relative already; NASM's is absolute without it).
2. Write `"..."` strings with C escapes: `"\n"` is a newline here. A NASM
   string with a backslash meant literally needs `\\`.
3. Say every memory size where the instruction reads several and NASM would
   have guessed (`cvtsi2ss`, x87 loads and stores).
4. `%include` and `incbin` find files by name and tags, not by path: write
   the name and, if needed, a tag it wears or `everywhere`.
5. Put `endbr64` at every label whose address the program takes as a value.
6. Assemble; each refusal says what to write instead.

## Every difference

- Strings in `"..."` take C escapes. NASM gives escapes only to backquoted
  strings.
- `/ % >>` are signed, as in C. In NASM they are unsigned (`//` and `%%` are
  its signed forms).
- `near` keeps a jump rel32. NASM keeps it only with `strict near`.
- `align` in a data section pads with zeros. NASM pads with `nop` unless
  told `db 0`.
- `[label]` is RIP-relative without `default rel`.
- `[rip + x]` is accepted as a spelling of `[rel x]`.
- A short jump, `loop` or `jrcxz` reaches only a label of its own section;
  NASM gives one to another section or object an R_X86_64_PC8 relocation.
- `lock` where it cannot go, `rep` before `cmps` or `scas`, and a prefix on
  a line of its own are refused. NASM warns of the first and takes the others.
- A decimal floating-point constant exactly halfway between two values of
  its width rounds to the even one, as IEEE 754 and C compilers round it.
  NASM 3.01 rounds it down (`dd 16777219.0` is 0x4B800002 here, 0x4B800001
  there); `tools/boxasm_oracle.py floats` holds BoxASM to the exact value.
- A floating-point constant too large for its width, or rounding to zero,
  is refused; NASM warns and writes infinity, or writes zero.
- `cvtsi2ss` and `cvtsi2sd` from memory, and their `v` forms, need its size
  (`dword` or `qword`); NASM takes unsized memory for a dword.
- `vcmpss`, `vcmpltss` and their kin read 4 bytes of memory, as the manual
  says, and take `dword`; NASM 3 refuses `dword` and takes `qword`.
- An immediate a form keeps in one byte is -128 to 255; NASM warns of
  another and keeps its low byte.
- The destination, index and mask of a gather that are not three different
  registers are refused; NASM assembles them, and the processor stops the
  program.
- An operand only AVX-512 gives a form for (`vpbroadcastd xmm0, eax`) is
  refused; NASM writes the EVEX form, which a processor without AVX-512
  cannot run.
- `sarx shlx shrx` take a count register of the operation's size, as the
  manual writes them; NASM takes a general register of any size there.
- `bextr` with an immediate is AMD's TBM, not built; NASM writes it.
- x87 memory an instruction reads in several sizes must say which; NASM
  takes a dword where none is said (a word for `fisttp`).
- A memory operand of one size (`fbld`, `fldcw`, MMX's mm/m64) said to be
  of another, or the x87 environment and state and fxsave's 512 bytes said
  to be of any, is refused; NASM ignores what is said.
- `st(1)` is refused, naming `st1`; NASM's `fadd to st1` is not built (write
  `fadd st1, st0`), nor are the 8087's and 287's `fdisi feni fsetpm`.
- `movd` takes 32 bits; NASM also takes a 64-bit register with an mm one,
  which is `movq`.
- `pinsrw` takes a 32-bit register or a word of memory, as the manual
  writes it; NASM also takes a 16-bit register.
- Packed SSE memory through a label not proven on a 16-byte boundary is
  refused; NASM assembles it, and the program stops when it runs.
- Hexadecimal floating-point constants, `__float32__()` and the other float
  functions of NASM are not built.
- The preprocessor computes `/ % >>` signed, as the assembler does. A macro
  called with a count no overload takes is refused; NASM leaves the line as
  it was (with a warning) unless the name is also an instruction.
- `struc` gives back the scope of `.local` names it found; NASM leaves its
  name as the scope. `at` is a directive only inside an istruc.
- `%include` and `incbin` find a file by name and tags in the context,
  never by a path, and take exactly one; NASM searches directories and `-I`
  paths and takes the first. `%include <name>` is refused with the line to
  write.
- An incbin skipping past the end of its file, or skipping or taking fewer
  than 0 bytes, is refused; NASM writes nothing, or refuses the skip only.
  An incbin in .bss is refused; NASM warns and leaves it out.
- A context `%push` opened and no `%pop` closed is refused at the end; NASM
  leaves it. `%substr` from before the first byte is refused.
- `%ifmacro` finds an `%imacro` in any case, as a call does; NASM 3 finds it
  only in its own. Of a `%define` and an `%idefine` of one name, the
  `%define` is found by its own spelling; NASM takes the newer.
- Not built of NASM's preprocessor: `%defstr %deftok`, `%[...]`, `%line`,
  `%pragma`, `%depend`, `%pathsearch`.

- Names are made of letters, digits, `_`, `.` and `@`, and begin with a
  letter, `_` or `.`; NASM also takes `$ # ~ ?` in them.

Everything else in the shared subset assembles to the same bytes and
relocations: `tools/boxasm_oracle.py` generates a corpus of about 36,000
lines and compares the two objects.
