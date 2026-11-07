# The preprocessor

Before the assembler reads a word, a preprocessor carries out NASM's
directives, line by line, the way NASM's does: macros of one line and of
many, conditions, repetition, contexts, string functions, and the files a
source takes in. This chapter is its specification; the shared part is held
to NASM's own preprocessor by `tools/boxasm_oracle.py macros`, a program
using every feature both have, whose object must equal NASM's.

## Directives

- `%define NAME text`, `%define NAME(a, b) text` (the parentheses right after
  the name; overloads by the number of arguments), `%xdefine` (expanded where
  written), `%assign NAME expression`, `%undef NAME`. A macro is expanded
  again in what it expands to, but never inside its own expansion.
  `__LINE__` and `__FILE__` are there unless defined. `%idefine`,
  `%ixdefine` and `%iassign` define a name that matches in any case
  (`%idefine Size 4` is `SIZE` and `size` too); `%undef` takes it away in
  any case.
- `%macro NAME 2`, `1-3` (with defaults after the count: `%macro m 1-3 20,
  30`), `0-*`, and `+` for a last parameter that takes the rest of the line;
  `%endmacro`, `%unmacro NAME count`. In the body: `%1`, `%{10}`, `%0`
  (the count, defaults included), `%%label` (a label of each call's own),
  `%+` (joins what stands on either side), `%rotate n`. Parameters are split
  at commas; `{a, b}` is one. Overloads take counts of their own. A macro
  named like an instruction stands aside for a count it does not take.
  A call may follow a label: `start: prologue`. `%imacro` defines one called
  in any case, `%unimacro` takes it away.
- `%if expression`, `%elif`, `%else`, `%endif`; the expression takes numbers
  and characters, `+ - * / % << >> & | ^ ~`, comparisons `== = != <> < <= >
  >=`, and `! && ^^ ||`. Tests: `%ifdef NAME`, `%ifmacro NAME [count]`,
  `%ifidn a, b` and `%ifidni` (the same tokens; any case), `%ifnum`,
  `%ifstr`, `%ifid`, `%ifctx name…` (the context on top is one of them).
  Each has its `%ifn…` and `%elif…` forms.
- `%rep count` … `%endrep`, and `%exitrep` to leave it.
- `%push [name]` opens a context, `%pop [name]` closes the one on top (the
  name, when written, must be its), `%repl name` renames it. `%$name` is a
  name of the context on top — a label, or a macro `%define %$x 1` — and
  `%$$name` one of the context below it: each is a name of its own, as
  `%%label` is a call's. A `%define` whose body holds one names the context
  it is expanded in. Contexts are the whole source's: one opened in a macro
  may be closed outside it; one left open at the end is refused.
- `%strlen NAME string` defines NAME as how many bytes the string holds;
  `%substr NAME string, start[, length]` as the bytes from the start'th (the
  first is 1), one when no length is written, and to the end and one short of
  it for -1 and -2; `%strcat NAME string, string...` as the strings joined.
  Strings are read as the assembler reads them, escapes and all.
- `%error text` (or `%fatal`) refuses with the text; `%warning text` says it
  (`prog.asm:4: warning: text`, before what `assemble` says of the build)
  and the work goes on.
- `%include "name"` reads another file where the line stands (see
  Including a file).

### An example: structured macros on contexts

A context gives each use of a macro pair names of its own, so pairs nest:

```nasm
%macro repeat 0
    %push repeat
%$begin:
%endmacro

%macro until_zero 1
    dec %1
    jnz %$begin
    %pop repeat
%endmacro

    mov ecx, 5
    repeat
        mov edx, 3
        repeat
            ; ...
        until_zero edx
    until_zero ecx
```

## Errors and limits

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

## Including a file: %include

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

## Including a file's bytes: incbin

`incbin` lays out the bytes of a file, as `db` lays out a string's. The file
is found as `%include` finds one — by its name and tags, `everywhere` past
the home — and after it may stand how many bytes to skip and how many to
take:

```text
logo: incbin "logo.bin"                        the whole file
part  incbin "table.bin" data, 16, 64          64 bytes past the first 16
rest: incbin "table.bin" data everywhere, 16   all past the first 16
      times 2 incbin "beep.bin"
```

The skip and the count are numbers known where the line stands. A count
past the end takes what there is; a skip past the end, a number below 0, or
an incbin in .bss is refused. A file is at most 4 GiB, and so are the bytes
of all the program's strings and files together.
