# BoxOS Command Grammar v0.1

Status: draft. This is a design record only; it is not an implementation specification yet.

## What is built (2026-10-29)

The owner and Claude settled the part below after a long survey; it is
implemented and supersedes the draft where they differ (parentheses,
commas as sequences, several arrows).

**The rule.** The words of a command come in any order. The shell sorts
each by what it IS — a file that is here (in the Use Context), `them` or
`it` (what the last command gave back), `all` (everything here), or a plain
word — into the fields the program's **Form** declares, in their order.
`everywhere` makes the names after it look past the context. A comma joins
names into one list. A line is a step. Nothing else is special: no flags,
no parentheses, no connecting words, and `:` stays the tags' own.

**The Form.** A program says what it takes and what it gives, in its image
(section `.boxos.form`): `takes [optional] thing|things|name|words|text|into
"what it is"` and `gives things|text` — `BOX_FORM(...)` in C
(`box/form.h`), `box::form` in C++ (`box/cxx/form.h`), the `takes`/`gives`
directives in BoxASM. The shell knows only Forms: a utility and a program a
person wrote yesterday are filled the same way. The program reads its
fields filled (`form_things`, `form_words`, `form_text`) and never parses
its line; started by another program, BoxLib fills them itself by the same
rule, asking nobody. `help NAME` shows a program's Form.

**The arrow.** `->` puts the result into a thing: into the program's `into`
field when it has one (`assemble x.asm -> x.elf`), otherwise everything the
program prints is added to the end of that file, made when it is not there
(`info -> machine.txt`). Questions the program asks the person still go to
the screen.

**Souvenir.** A program gives files back (`souvenir_things`); the shell
keeps them as `them`/`it`: `files draft` then `tag them done`;
`assemble planets.asm` then `run it`. It goes only to a starter that asked
for it (the `souvenir` record in the Luggage): the shell asks when it will
keep it, and a program started by anyone else gives back to no one.

**Recipe.** A program made here remembers what it was made from: `assemble`
writes into the image every file it read (by id, with its SHA-256), the Use
Context it looked in, and its own name. `info planets.elf` shows them and
which changed since. Started from the shell — by name or with `run` — a
program whose files changed is made again first ("planets: planets.asm
changed since it was made; making it again"), in the context it was made in;
when that fails nothing starts. A file that is gone makes nothing: the program
starts as it was made, and the person is told. `assemble planets.elf` makes
a program again by its Recipe without starting it.

**Itinerary.** Steps of the shell kept in a file that wears `itinerary`: its
first lines are its Form (`takes`, `gives`), every line after them a step, as
if typed. It is started by name like any program — the same sorting, the same
questions, `help NAME` shows it. A field's NAME (`takes words label "the tag
they get"`) is, in every step, what the field was given: files where files
are taken, words where words go. `them` begins as its first things field and
goes on as the steps give; with `gives things`, `them` at the end is what it
gave. A step that does not go through stops it ("tidy: step 2 (tag files
label) did not go through; the rest is not done"). It has no conditions, so
one that would run itself again is not started. An arrow on it goes into its
`into` field; with none it is refused.

**Lists.** A comma list is a list of things: `them`, `it`, `all` and an
Itinerary's field names mean in it what they mean alone (`tag a.txt, them
done`); quoted, they are plain words. A file is in a field once, however many
words name it.

```text
takes thing source "the source"        tidy n1.txt, n2.txt done
assemble source                        try planets.asm
run it
```

**Asking.** Only when it must, before the program starts: a name that is
more than one file ("which one? (a number)"), a required field left empty
("tag: the tags they get?", with "there is no helo.asm here" for a typo),
`all` with no context ("all is the whole volume here: 92 files. sure?").
An empty answer starts nothing.

```text
tag a.txt done              tag done a.txt, b.txt       tag them done
rename note.txt idea.txt    create idea.txt work        files draft old
say hello world -> notes.txt                            info -> machine.txt
assemble planets.asm        run it                      help tag
```

## 1. Basic form

```text
command arguments (clarifications) -> result
```

Examples:

```text
files hello
create note hello test
rename note -> journal
assemble program.asm -> program.elf
```

A command may be a built-in action or a program name:

```text
lavalamp
draft
info
reboot
```

## 2. Sequences

The preferred short form is comma-separated:

```text
create note, tag note hello test
```

The following forms are also allowed:

```text
create note and tag note hello test
```

```text
create note
tag note hello test
```

All three forms describe a sequence of actions.

## 3. Arguments and tags

Ordinary words are used directly:

```text
files hello test
cube red 10
tag note hello test fun
```

Tags remain one open user-visible vocabulary. They are not divided into user-facing categories.

`Use Context` is automatically added to relevant queries:

```text
use code cpp
files project
```

This means `code AND cpp AND project`.

Clear the context with:

```text
use
```

Query outside the current context with:

```text
files project (everywhere)
```

## 4. Parentheses

Parentheses are universal clarifications. Their meaning is interpreted by the command:

```text
cube (color red) (size 10)
draft file (tag1, tag2)
show note (name, size, tags)
assemble program.asm (errors)
tool (tag1)
```

When the meaning is obvious, parentheses may be omitted:

```text
cube red 10
tool tag1
```

Spaces are the basic separator inside parentheses. Commas are allowed for readability:

```text
(color red size 10)
(color red, size 10)
```

## 5. The arrow

The main arrow is `->`. It expresses a result, transformation, or destination:

```text
compile main.cpp -> main
rename note -> journal
copy note -> note2
files hello -> report (file)
```

A readable long form may also be accepted:

```text
compile main.cpp to main
```

Multiple specialized arrows (`=>`, `|>`, `~>`, and similar) are not part of the base standard for now.

## 6. Selecting an entity

When a name resolves to one entity, the system acts immediately:

```text
draft note
```

When several entities match:

```text
draft file
```

BoxOS shows useful identifying information, such as names and tags, and asks the user to choose.

Explicit selection can use tags or exclusions:

```text
draft file (tag1, tag2)
delete file (not tag3)
tool (tag1)
```

The current context can also select the entity:

```text
use tag1 tag2
draft file
```

## 7. Defaults and settings

If a value can be inferred, the user does not need to provide it:

```text
assemble program.asm
```

The result may default to `program.elf`.

Persistent behavior belongs to the program's settings:

```text
cube
```

One-time intent may be expressed in the command:

```text
cube (random color)
cube (without shadows)
```

## 8. Name resolution

For a bare name, the shell:

1. checks built-in commands;
2. searches for a program by name and tags;
3. asks the user if a conflict remains;
4. reports an unknown name in human language when nothing matches.

Example:

```text
What? lavalamp? I don't know what that is.
```

## 9. Diagnostics

Errors are shown automatically. The user does not need to add `(errors)`:

```text
assemble program.asm
```

Optional explicit forms may request a diagnostic view:

```text
assemble program.asm (errors)
assemble program.asm (show errors)
```

## 10. Atomicity

A single coherent operation is atomic where the underlying operation supports it:

```text
tag note hello test fun
```

Either all requested tags are added or none are.

A sequence of different actions may partially complete:

```text
create note, tag note hello test
```

If a later action fails, the system reports what already happened and may ask how to continue.

## 11. Existing-style examples

```text
lavalamp
files hello
use code cpp
files project
use
create note hello test
tag note fun
untag note test
show note (name, size, tags)
draft note
rename note -> journal
copy note -> note2
assemble program.asm
assemble program.asm -> program.elf (tags app test fun)
files hello -> report (file)
reboot
bye
sleep
info
help assemble
```

## 12. Open questions

- Final semantics and naming for `trash`, `delete`, and `erase`.
- A settings format shared by programs.
- Quoting and names containing spaces.
- Exact disambiguation between a program argument and a tag selector.
- The final natural-language command surface.
- Which existing utility names should be migrated or removed.
- How executable eligibility should relate to current `app` and `utility` tags.
