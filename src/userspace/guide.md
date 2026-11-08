# A Short Guide to BoxOS

BoxOS keeps no folders. A file is **what it is** and *what it wears*: a name,
and tags such as `work`, `draft` or `project:alpha`. You find files by asking
for their tags, and the machine can be told what you are doing — the
**Use Context** — so that every program looks there first.

This guide is itself a file of the volume, written in Markdown. Read it with
`read guide.md`: headings, lists, quotes and code are shown as what they mean.

## Reading

`read` is for reading quietly; `edit` is for changing a file. The two are one
family: the same tabs, the same windows, the same keys for the same things.
`read` alone opens the file chooser, as `edit` does. The text stands in a
column across the middle of the glass, the line you are reading is lit, and
the bottom row says how far through you are and how long the rest takes.
Letters beyond ASCII that the console font holds are drawn as themselves:
café, naïve, Straße, α, π, ±, ≥ and ½.

| keys                       | what they do                         |
|----------------------------|--------------------------------------|
| Space, PgDn, Ctrl+Down     | the next page                        |
| Shift+Space, PgUp, Ctrl+Up | the page before                      |
| Down, Up                   | the next line, the one before        |
| Tab, Shift+Tab             | the next heading, the one before     |
| Ctrl+L                     | the contents                         |
| Ctrl+F                     | find                                 |
| Ctrl+M, Ctrl+Shift+M       | a bookmark here, all the bookmarks   |
| Ctrl+=, Ctrl+-             | a wider column, a narrower one       |
| Ctrl+C                     | copy the paragraph or the selection  |
| Ctrl+N, Ctrl+1..0          | open a file in a new tab, go to one  |
| Ctrl+?                     | every key                            |

A book opens where you left it. Where you stopped and the bookmarks you put
are kept in one file of their own, `read.marks`. A bookmark keeps the words it
was put on, so when the file is changed it goes where they went; the files
you read are never written.

## Finding

`seek` looks into the text of files:

```
seek TODO                  the files of the Use Context that say TODO
seek TODO urgent           that say both words, anywhere in them
seek "x == 1"              that say it as one phrase
seek TODO them             among what the last command gave
seek TODO everywhere all   on the whole volume
```

What `seek` finds, it gives back: the next command can say `them`.

> Commands give and take files. `seek TODO` and then `edit them` opens
> every file that still has something to do.

## Changing tags

Tags grow. `retag` changes one tag on many files at once, and shows you what
will change before it changes anything:

1. `retag project:old project:new` — that tag becomes that one
2. `retag project:... proj:...` — the key is renamed, every value kept
3. `retag draft:... status:draft` — every tag of the key becomes the one

Nothing happens until you answer *yes*, and the files changed are given back,
so the same command the other way round takes it back.

## The clipboard

BoxOS keeps everything you copy, not only the last thing:

- **Ctrl+C**, **Ctrl+X** — copy, cut
- **Ctrl+V** — paste the newest clip
- **Ctrl+Shift+V** — choose any clip from the list
  - the window opens on the one before the newest
  - pasted one after another, clips come in the order they were copied

A password can be copied to be taken once, or to last only as long as the
program that copied it. When it goes, its bytes are wiped.

### Keys that are the same everywhere

Every program says these keys the same way. `Ctrl+C` copies — it never stops
a program.

---

*Read on with Tab, or press Ctrl+L for the contents. Esc leaves.*
