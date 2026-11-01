# BoxASM — the assembler inside BoxOS

Status 2026-11-01: implemented in `src/userspace/utils/assemble/`, held to
NASM 3 byte for byte and to GNU ld by `tools/boxasm_stand.sh` (the `boxasm`
gate), run end to end in BoxOS (`hello`, `countdown`). The instructions are
x86-64's general ones, SSE to SSE4.2, and AVX, AVX2, FMA, F16C, BMI1, BMI2
and MOVBE — x86-64-v3's sets — but for x87 and MMX.

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

Assembling again replaces the program: the file of that name wearing every
tag the new one will wear is put out of the way once the new one is written
whole, and its other tags (`favorite`, `autostart`) pass to the new one. When
that is not one file, `assemble` asks:

```text
there is another hello.elf, tags hello,code,asm,app
replace it? (yes, or no to make a new one beside it)

hello.elf could mean any of these:
  1: tags hello,code,asm,example,app
  2: tags hello,code,asm,test,app
which one does the program replace? (a number, or new)
```

A tag in the command names the one meant without a question:
`assemble hello.asm (tags test)`.

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
- Floating-point constants go in `dd` (32-bit) and `dq` (64-bit):
  `dq 3.14159, -2.5, 1.e10, 6.022e23`, `dd 0.1`. The `.` is what makes one
  (`dd 1` is an integer); `_` may stand among the digits. `__Infinity__`,
  `__QNaN__` (or `__NaN__`) and `__SNaN__` are the special values. Each is
  rounded straight from its digits to the nearest value of its width, ties
  to even. One too large for its width, or not zero but rounding to zero, is
  refused. Instructions take no floating-point immediates: a constant is put
  in data and loaded from there (`movsd xmm0, [pi]`).

### Expressions

`| ^ & << >> + - * / %`, unary `- ~ +`, parentheses, with NASM's precedence.
They are computed as signed 64-bit integers, the way C computes them. An
address takes only `+` and `-` of a number. Two addresses in one section
subtract to a number.

### Operands

- Registers: `rax..r15`, `eax..r15d`, `ax..r15w`, `al..r15b` (also `r8l`),
  `spl bpl sil dil`, and `ah bh ch dh` (which no instruction needing REX may
  use); `xmm0..xmm15` for SSE, `ymm0..ymm15` for AVX.
- Memory: `[base + index*scale + displacement]`. The scale (1, 2, 4, 8) may
  be written first: `[8*rcx + table]`.
- `[label]` is RIP-relative. `[abs label]` is the 32-bit absolute address,
  and `[rip + label]` is the same as `[rel label]`. An address made of a bare
  number is absolute.
- `byte word dword qword oword yword` (1 to 32 bytes) say the size of a
  memory operand where no register does: `inc qword [rax]`. Instructions with one size need none:
  `push [rax]`, `jmp [rbx]`, `sete [rcx]`, `cmpxchg16b [rdi]`.
- `short` and `near` before a jump target force rel8 or rel32. Without them,
  the assembler takes the shortest form that reaches.

### Instructions

- Moves and arithmetic: `mov movzx movsx movsxd lea xchg push pop add or adc
  sbb and sub xor cmp test inc dec not neg mul imul div idiv`.
- Shifts and rotates: `rol ror rcl rcr shl sal shr sar shld shrd`.
- Bits: `bt bts btr btc bsf bsr popcnt lzcnt tzcnt bswap`.
- Branches: `jmp call ret`, `jcc setcc cmovcc` with every condition
  spelling, and `loop loope loopz loopne loopnz jrcxz jecxz`, which reach
  only -128 to 127 bytes.
- Strings: `movs stos lods cmps scas`, each with `b w d q`.
- Atomics: `xadd cmpxchg cmpxchg8b cmpxchg16b`; fences `lfence sfence mfence`.
- The processor: `cpuid rdtsc rdtscp rdrand rdseed`.
- SSE and SSE2, which every x86-64 processor has:
  - `add sub mul div min max sqrt` with `ps pd ss sd`; `rcpps rcpss rsqrtps
    rsqrtss`; `and andn or xor unpckl unpckh` with `ps pd`; `shufps shufpd`;
  - compares `cmpps cmppd cmpss cmpsd` with an imm8, and `cmp` + `eq lt le
    unord neq nlt nle ord` + `ps pd ss sd` (`cmpltsd`); `comiss comisd
    ucomiss ucomisd`;
  - moves `movaps movapd movups movupd movss movsd movlps movhps movlpd
    movhpd movhlps movlhps movdqa movdqu movd movq movmskps movmskpd
    pmovmskb movntps movntpd movntdq movnti maskmovdqu`;
  - conversions `cvtsi2ss cvtsi2sd cvtss2si cvttss2si cvtsd2si cvttsd2si
    cvtss2sd cvtsd2ss cvtps2pd cvtpd2ps cvtdq2ps cvtps2dq cvttps2dq cvtdq2pd
    cvtpd2dq cvttpd2dq`;
  - packed integers `padd psub` (`b w d q`), `padds paddus psubs psubus`
    (`b w`), `pmullw pmulhw pmulhuw pmuludq pmaddwd pavgb pavgw pmaxsw
    pmaxub pminsw pminub psadbw pand pandn por pxor pcmpeq pcmpgt` (`b w d`),
    `packsswb packssdw packuswb punpckl punpckh` (`bw wd dq qdq`), shifts
    `psll psrl` (`w d q`) and `psra` (`w d`) by xmm or imm8, `pslldq psrldq`,
    `pshufd pshufhw pshuflw pextrw pinsrw`;
  - `ldmxcsr stmxcsr prefetchnta prefetcht0 prefetcht1 prefetcht2 clflush`.

  `movsd` and `cmpsd` without operands are the string instructions; with
  them, SSE2's. An SSE operand of memory reads 4 (`ss`), 8 (`sd`, `movq`) or
  16 bytes (`oword`); a size written must be that one.

  Packed operations need their 16 bytes of memory on a 16-byte boundary, or
  the processor stops the program; only `movups movupd movdqu` read memory
  anywhere. When the memory is a label of the program, the assembler proves
  the boundary — the label's place a multiple of 16 in a section something
  said `align 16` in — or refuses the line. Memory through a register is the
  program's to align.

  The C call ABI passes `double` and `float` arguments in `xmm0..xmm7` and
  returns one in `xmm0`; every xmm register may change across a call, and
  `al` counts the vector registers a variadic callee receives. A `float`
  goes to a variadic function such as `printf` widened to a `double`
  (`cvtss2sd`). BoxLib's `printf` writes C's whole set of conversions, `%f
  %e %g %a` among them, each value's digits exact and rounded to nearest:
  `fractions.asm` prints its results that way.
- SSE3, SSSE3, SSE4.1 and SSE4.2:
  - `addsubps addsubpd haddps haddpd hsubps hsubpd lddqu movddup movshdup
    movsldup monitor mwait`;
  - `pabsb pabsw pabsd phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw
    pmulhrsw pshufb psignb psignw psignd palignr`;
  - `blendps blendpd pblendw` and `blendvps blendvpd pblendvb`, whose mask is
    xmm0, written third or left out; `dpps dppd mpsadbw extractps insertps
    movntdqa packusdw pcmpeqq phminposuw ptest pmuldq pmulld`, `pmin` and
    `pmax` with `sb sd uw ud`, `pextrb pextrd pextrq pinsrb pinsrd pinsrq`,
    `pmovsx` and `pmovzx` with `bw bd bq wd wq dq`, `roundps roundpd roundss
    roundsd`;
  - `crc32` of a byte, word, dword or qword, `pcmpestri pcmpestrm pcmpistri
    pcmpistrm pcmpgtq`.
- AVX and AVX2, in the VEX encoding:
  - the `v` form of each SSE to SSE4.2 instruction above but `movnti`, the
    prefetches, `clflush`, `monitor`, `mwait` and `crc32`, which have none,
    with a destination of its own: `vaddps
    xmm1, xmm2, xmm3`. Where the second operand is left out it is the first
    again: `vaddps xmm1, xmm2` is `vaddps xmm1, xmm1, xmm2`. AVX widens the
    floating-point operations and the moves to ymm, AVX2 the integer ones;
    the others stay on xmm and say so when given a ymm;
  - `vcmpps vcmppd vcmpss vcmpsd` with an imm8, and `vcmp`, one of the 32
    predicates by name, then `ps pd ss sd`: `vcmplt_oqpd`, `vcmpneq_uqss`;
  - AVX's own: `vbroadcastss vbroadcastsd vbroadcastf128 vextractf128
    vinsertf128 vmaskmovps vmaskmovpd vpermilps vpermilpd vperm2f128 vtestps
    vtestpd vzeroupper vzeroall`; `vblendvps vblendvpd vpblendvb` take the
    register they select by last;
  - AVX2's own: `vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq
    vbroadcasti128 vextracti128 vinserti128 vperm2i128 vpermd vpermps vpermq
    vpermpd vpblendd vpmaskmovd vpmaskmovq vpsllvd vpsllvq vpsravd vpsrlvd
    vpsrlvq`;
  - the gathers `vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd
    vpgatherdq vpgatherqd vpgatherqq`, whose address is indexed by a vector
    register: `vpgatherdd ymm0, [rax + ymm1*4], ymm2`. The destination, the
    index and the mask are three different registers, or the processor stops
    the program, so the assembler refuses them otherwise. A vector register
    indexes no other address.

  Of AVX's memory operands only those of `vmovaps vmovapd vmovdqa` and the
  non-temporal moves must be on a boundary of their width, 16 or 32 bytes;
  the assembler proves it as it does for SSE.
- FMA: `vfmadd vfmsub vfnmadd vfnmsub` with `132 213 231` and `ps pd ss sd`,
  and `vfmaddsub vfmsubadd` with `132 213 231` and `ps pd` — a product and a
  sum rounded once. The digits say which operands are multiplied: 132 is
  `first = first * third + second`, 213 is `first = second * first + third`,
  231 is `first = second * third + first`. All three are always written.
- F16C: `vcvtph2ps` turns four or eight half-precision numbers into singles,
  `vcvtps2ph` turns them back, rounded as its imm8 says.
- BMI1 and BMI2, on 32- and 64-bit registers: `andn bextr blsi blsmsk blsr
  bzhi mulx pdep pext rorx sarx shlx shrx`. Of those with three operands the
  second may be left out: `shlx eax, ecx` is `shlx eax, eax, ecx`.
- `movbe` loads or stores with the bytes reversed; `lahf` and `sahf` move the
  flags through ah; `xgetbv` reads the register ecx names — with 0, XCR0,
  whose bits 1 and 2 say the system saves the xmm and ymm registers.
- Alone: `nop int3 leave cqo cdq cwd cdqe cwde cbw pushfq popfq cld std clc stc
  cmc endbr64 pause ud2`.

Each encodes as NASM encodes it.

`popcnt` needs CPUID.1:ECX bit 23, `lzcnt` the LZCNT bit of leaf 0x80000001,
`tzcnt` BMI1, `rdrand` and `rdseed` their own bits, `cmpxchg16b` CX16. So
does each set: SSE3, SSSE3, SSE4.1, SSE4.2, MOVBE, FMA, AVX and F16C in
CPUID.1:ECX (bits 0, 9, 19, 20, 22, 12, 28, 29), AVX2, BMI1 and BMI2 in
CPUID.7:EBX (bits 5, 3, 8), `lahf` and `sahf` in bit 0 of ECX of leaf
0x80000001. An instruction encoded with VEX also needs the system to save
the ymm registers: CPUID.1:ECX bit 27 (OSXSAVE), then `xgetbv` with ecx 0
and bits 1 and 2 of eax. BoxOS saves them.
The assembler cannot know the machine a program will run on: a processor
without `lzcnt` or `tzcnt` runs them as `bsr` and `bsf`, which answer
differently, and one without the others stops the program. Ask `cpuid`
first, as `machine.asm` does with `bt ecx, 23`.

### Prefixes

`lock` goes before an instruction the processor lets lock (`add adc and btc
btr bts cmpxchg cmpxchg8b cmpxchg16b dec inc neg not or sbb sub xadd xchg
xor`) whose destination is memory. `rep` goes before `movs stos lods`;
`repe`/`repz` and `repne`/`repnz` before `cmps scas`. A prefix and its
instruction share a line. Any other pairing is refused: the processor
would reject it, or read it as another instruction (`rep bsf` is `tzcnt`).

### Macros

A preprocessor carries out NASM's directives before the assembler reads a
word, line by line, the way NASM's does:

- `%define NAME text`, `%define NAME(a, b) text` (the parentheses right after
  the name; overloads by the number of arguments), `%xdefine` (expanded where
  written), `%assign NAME expression`, `%undef NAME`. A macro is expanded
  again in what it expands to, but never inside its own expansion.
  `__LINE__` and `__FILE__` are there unless defined.
- `%macro NAME 2`, `1-3` (with defaults after the count: `%macro m 1-3 20,
  30`), `0-*`, and `+` for a last parameter that takes the rest of the line;
  `%endmacro`, `%unmacro NAME count`. In the body: `%1`, `%{10}`, `%0`
  (the count, defaults included), `%%label` (a label of each call's own),
  `%+` (joins what stands on either side), `%rotate n`. Parameters are split
  at commas; `{a, b}` is one. Overloads take counts of their own. A macro
  named like an instruction stands aside for a count it does not take.
  A call may follow a label: `start: prologue`.
- `%if expression`, `%elif`, `%else`, `%endif`; the expression takes numbers
  and characters, `+ - * / % << >> & | ^ ~`, comparisons `== = != <> < <= >
  >=`, and `! && ^^ ||`. Tests: `%ifdef NAME`, `%ifmacro NAME [count]`,
  `%ifidn a, b` and `%ifidni` (the same tokens; any case), `%ifnum`,
  `%ifstr`, `%ifid`. Each has its `%ifn…` and `%elif…` forms.
- `%rep count` … `%endrep`, and `%exitrep` to leave it.
- `%error text` (or `%fatal`) refuses with the text.
- `%include "name"` reads another file where the line stands (see
  Including a file).

`struc NAME` … `endstruc` writes a layout: its labels (`.x` is `NAME.x`) are
offsets from 0, made by `resb resw resd resq` and `align` (or `alignb`), and
`NAME_size` is where it ends. They are constants: `mov eax, [rbx + point.y]`,
`add rbx, point_size`. `istruc NAME` … `iend` lays one out in data: `at
point.y, dd 2` fills with zeros up to the field and writes it there, and iend
fills to `NAME_size`.

An error in a macro's expansion is said at the line of the macro that made
it, the line shown as it expanded, then each call and `%include` it was
reached through; a macro calling itself from one line is said once, with how
many calls deep:

```text
prog.asm:2:14: 'nowhere' is never defined; ...
        mov eax, nowhere
                 ^
    in the macro 'load' called at line 5
    in the macro 'twice' called at line 10
```

How deep a source nests — macro calls in macro calls, `%define`s in
`%define`s, files in files, parentheses, names defined by names — is measured
on the stack: a BoxOS program's stack grows to 8 MiB, `assemble` takes all
but 24 KiB of it, and a source past what fits is refused in words at the
line where it ran short (a recursive macro is named:
`'rec' is called inside macro calls too deep: a macro may be calling
itself`). The expanded source reaches 16 MiB and the preprocessor walks 50
million lines. `macros.asm` shows most of this at work.

### Including a file

A file is found as BoxOS finds a file — by its name and the tags it wears,
never by a path:

```text
%include "math.asm"                       the one called so, here
%include "math.asm" fast                  ... that wears fast as well
%include "print.asm" mylib everywhere     past the context: the one wearing mylib
```

- Here is the home of the file the line is written in. The source's home is
  the Use Context the build began in, taken once, so a context said on
  another console meanwhile moves nothing; a file found beside it has the
  same home.
- The words after the name are the meaning the file must wear, checked
  whenever they are written: a file that does not wear them is not the one.
- `everywhere` looks past the home. A file reached so finds its own
  `%include`s among the tags of the line that reached it, so a library's
  neighbours are its own wherever it is used.
- Exactly one file: two that fit, or none, refuse, saying what is there and
  what to write:

```text
prog.asm:5:10: math.asm could be any of 2 files in demo
    %include "math.asm"
             ^
  say which by a tag only it wears:
    %include "math.asm" fast    for the one wearing math, demo, fast
    %include "math.asm" old     for the one wearing math, demo, old

prog.asm:5:10: there is no lone.asm in demo
    %include "lone.asm"
             ^
  outside it, reached by:
    %include "lone.asm" game everywhere    for the one wearing lone, game
  or let it wear demo too
```

- A file included again while it is being read is refused (the files
  include one another in a circle); a file holding a NUL byte is no source.
  An included file is a body of its own: a `%if` or `%macro` opened in it is
  closed in it, and it is outside the macro call and `%rep` of its line.
- An error in an included file names that file and the line that included
  it: `in the file 'math.asm' included at line 3 of prog.asm`.
- `assemble` finds the source itself the same way and says the same when it
  cannot: which files of that name there are, and that `use` narrows or
  changes the context.

### Form: what the program takes and gives

A program tells the shell what it takes, so the shell sorts the words a
person typed into the program's fields, in their order, and asks only when it
must (see `box/form.h`):

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
  `istruc`, and in any section; the fields keep the order they are written
  in, wherever the directives stand relative to code.
- The kind is one of `thing`, `things`, `name`, `words`, `text`, `into`; the
  words in quotes (a few words, none empty, with no newline, NUL or double
  quote) are what the shell says when it asks for the field.
- A Form holds at most 16 fields, `into` once, `gives` once.
- The assembler writes the Form as the section `.boxos.form`: the line
  `form 1`, then one line per directive, as the C macro `BOX_FORM` does. The
  linker puts it read-only after `.nameplate`, so the shell finds it by name
  in the file, and defines `__boxos_form_start` and `__boxos_form_end` around
  it (equal when the program has no Form).

```text
takes optional things "the files"
takes words "the tags they get"
gives text
```

becomes `form 1\ntakes optional things "the files"\ntakes words "the tags they
get"\ngives text\n`.

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
- Packed SSE memory through a label not proven on a 16-byte boundary is
  refused; NASM assembles it, and the program stops when it runs.
- Hexadecimal floating-point constants, `__float32__()` and the other float
  functions of NASM are not built.
- The preprocessor computes `/ % >>` signed, as the assembler does. A macro
  called with a count no overload takes is refused; NASM leaves the line as
  it was (with a warning) unless the name is also an instruction.
- `struc` gives back the scope of `.local` names it found; NASM leaves its
  name as the scope. `at` is a directive only inside an istruc.
- `%include` finds a file by name and tags in the context, never by a path,
  and takes exactly one; NASM searches directories and `-I` paths and takes
  the first. `%include <name>` and `incbin` are not built.
- Not built of NASM's preprocessor: `%push`/`%pop` contexts and `%$` names,
  `%strlen %substr %strcat`, `%warning`, the case-blind `%idefine`
  `%imacro`, `%[...]`, `%line`, `%pragma`, `%depend`, `%pathsearch`.

Everything else in the shared subset assembles to the same bytes and
relocations: `tools/boxasm_oracle.py` generates a corpus of about 31,000
lines and compares the two objects.

## Not built yet

- x87 and MMX, `fxsave` and `fxrstor` (next); AVX-512; raw Manifest,
  `box.notify`, `box.wait`.
- Programs made of several sources; a native BoxOS object format.
