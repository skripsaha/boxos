# BoxOS Command Grammar v0.1

Status: draft. This is a design record only; it is not an implementation specification yet.

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
