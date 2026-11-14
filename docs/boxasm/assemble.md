# The assemble command

`assemble` is the program that turns a source into a program, inside BoxOS:
it assembles the source, links it with BoxLib, and writes the program as a
file of the volume, ready to be started by name.

## Using it

```text
use code asm example                      the Use Context: where files are looked for
assemble countdown.asm                    -> countdown.elf
assemble countdown.asm -> count.elf       the program named
assemble countdown.asm test               ... wearing the tag test too
countdown                                 start it
```

Its Form (see [language.md](language.md#the-form-what-a-program-takes-and-gives))
is:

```text
takes thing "the source"
takes optional into "the program"
takes optional words "the program's tags"
gives things
```

- **The source** is one file, found as the shell finds any file: by its name
  in the Use Context. When the name means several files, the shell asks
  which; when none is here, it says where there are some.
- **The program** is the name after `->`; without one it is the source's
  name with `.asm` replaced by `.elf`.
- **The tags** the program wears are the source's own, then the words of the
  command. When it would wear neither `app` nor `utility`, `assemble` adds
  `app` and says so: the shell starts nothing else.
- What it **gives** back is the program file, so a command that follows can
  take it as `it`.

Its output, when all goes well:

```text
assembled countdown.asm -> countdown.elf
tags: countdown,code,asm,example,app
added app: without app or utility the shell cannot start it
```

## Errors and warnings

A program that does not assemble or link leaves no file behind. An error
reads `file:line:column: what is wrong`, then the line and a caret under the
place, then each macro call and `%include` it was reached through:

```text
assemble: prog.asm:2:14: 'nowhere' is never defined; a name from outside the program is declared with extern
        mov eax, nowhere
                 ^
    in the macro 'load' called at line 5
```

The assembler says what to write instead wherever it can: a file asked for
by a name several files have is answered with the lines that pick each one,
an instruction with the wrong register width with the one that takes it.

What the source says with `%warning` comes first, as `file:line: warning:
text`, and the build goes on.

## Making a program again

Assembling again replaces the program. The file of that name wearing every
tag the new one will wear is put out of the way once the new one is written
whole, and its other tags (`favorite`, `fun`) pass to the new one.
When that is not one file, `assemble` asks:

```text
there is another hello.elf, tags hello,code,asm,app
replace it? (yes, or no to make a new one beside it)

hello.elf could mean any of these:
  1: tags hello,code,asm,example,app
  2: tags hello,code,asm,test,app
which one does the program replace? (a number, or new)
```

A tag in the command names the one meant without a question.

## The Recipe: a program remembers what it was made from

Every program `assemble` writes carries its Recipe in the section
`.boxos.recipe`: the tool that made it (`by assemble`), the Use Context the
build looked in, and every file the build read — the source first, then each
`%include` and `incbin` — by id, name and SHA-256 of the bytes read.

- **Started**, a program is checked against its Recipe. When a file it was
  made from changed since, the shell has `assemble` make it again first, in
  the context it was made in, and starts the new one: a person who changed a
  source and typed the program's name runs what the source says now. When the
  source is gone, the program starts as it was made, and the shell says so.
- **Handed to assemble** in place of a source (`assemble countdown.elf`), a
  program is made again from its Recipe, under its own name.

Programs built on the host, the system's own, carry no Recipe and start as
they are.

## try: assemble and run in one word

The example Itinerary `try` (tags `itinerary,example`) is two steps:

```text
takes thing source "the source"
gives things
assemble source
run it
```

`try prog.asm` makes `prog.elf` and starts it.

## What the program file holds

An ELF executable for BoxOS's loader: the program's code and data, BoxLib's
start code and the parts of `libbox.a` it uses, linked at the user image base;
`.nameplate`, the table of its function names the system reads to name a
place in a fault; `.boxos.form`, its Form; `.boxos.recipe`, its Recipe. See
[internals.md](internals.md) for the layout.
