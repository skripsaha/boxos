# painter::tui::text — Text in Cells

## Purpose

This module measures text, selects the longest prefix that fits, truncates strings,
and writes them into a `painter::tui::Frame`. Input strings contain UTF-8 bytes;
the rendered output consists of single-byte glyphs from the BoxOS font.

Painter is a family of graphics APIs. Shared coordinate and color types reside
in `painter::core`, while text and character-based graphics rendering reside in
`painter::tui`. `painter::gui` denotes the direction for a pixel-based GUI; no
implementation of it exists in the current source tree. This module belongs to
`painter::tui`.

Header: [`painter/tui/text.h`](../../../../src/userspace/painter/include/painter/tui/text.h).
Implementation: [`text.cpp`](../../../../src/userspace/painter/src/tui/text.cpp).
Code point-to-glyph conversion is defined in
[`boxos_glyph_fold.h`](../../../../src/include/boxos_glyph_fold.h).

Include: `#include "painter/tui/text.h"`. The functions are in the
`painter::tui` namespace and are part of `src/userspace/painter/libpainter.a`.
The examples below target C++23 and the BoxOS `boxcxx` and `boxlib` headers.

## Bytes, Code Points, and Cells

A string's size in bytes does not determine its on-screen width. `Length`
represents the number of bytes consumed, while `Width` represents the number
of cells occupied. `core::Point` coordinates in this module are also measured
in cells: `X` is the column and `Y` is the row; the frame origin is `{0, 0}`.

| Input | Handling | Cells | `Odd` |
| --- | --- | --- | --- |
| Printable ASCII, U+0020–U+007E | Corresponding glyph | 1 | `false` |
| `é` (U+00E9) | CP437 glyph `0x82` | 1 | `false` |
| `≠` (U+2260) | Two glyphs, `!=` | 2 | `false` |
| `—` (U+2014) | Horizontal line `0xC4` | 1 | `false` |
| Valid code point without a table entry | Square `0xFE` | 1 | `false` |
| Tab `\t` | Spaces up to the next tab stop | 1–4 | `false` |
| U+0000–U+001F except tab, and U+007F | Square `0xFE` | 1 | `true` |
| Invalid UTF-8 byte | Square `0xFE`; advance by one byte | 1 | `true` |

`TabStop` is a `std::size_t` constant equal to `4`. A tab occupies
`TabStop - Column % TabStop` cells: at a tab-stop boundary, it occupies four
cells rather than zero. `CellsOf`, `BytesWithin`, and `PutText` start counting
columns from zero on every call. In `PutText`, this count is independent of
`At.X`; two consecutive calls do not share tabulation state.

`OddInk` is a `core::Color` constant with the value
`core::Color::Rgb(0xE0, 0x9A, 0x30)`. `PutText` uses it for elements with
`Odd == true`. An unknown but validly encoded Unicode character is drawn as
a square in the regular `Ink` color. In particular, Cyrillic characters are
absent from the current table and are displayed as such squares.

The decoder rejects truncated sequences, stray continuation bytes, overlong
encodings, surrogates, and values above U+10FFFF. It does not combine code
points into grapheme clusters or perform normalization. Combining marks are
processed separately; width rules are defined by the BoxOS table.

The module does not parse ANSI sequences or wrap text. `\n` and `\r` become
squares rather than cursor-movement commands. A null byte inside an explicitly
specified `std::string_view` is likewise treated as a control character,
not as the end of the text.

## API Reference

### `Glyph`

**Summary:** Use this structure to represent a single text element in BoxOS cells.

**Fields:**

| Field | Type | Default | Purpose |
| --- | --- | --- | --- |
| `Length` | `std::size_t` | `1` | Number of input bytes to advance |
| `Width` | `std::size_t` | `1` | Number of cells occupied by the element |
| `Cells` | `char[2]` | `{' ', ' '}` | Glyph codes; not a null-terminated string |
| `Odd` | `bool` | `false` | Flag indicating a control character or invalid UTF-8 byte |

**Parameters:** This aggregate has no user-defined constructor; its fields can
be set through aggregate initialization.

**Returns:** Not applicable; this is a data structure.

**Side effects:** None.

**How it works:** `GlyphAt` fills the structure for one input element.
For a valid sequence, `Length` equals its number of bytes; for an error,
it remains one. For an ordinary element, the first `Width` entries of
`Cells` are used. A tab, however, is represented only by its width:
the two-element array does not hold all of its spaces.

**Notes:** When iterating manually, identify tabs by the original input byte
and output `Width` spaces. Without this check, do not read `Cells[Part]`
for every `Part < Width`: a tab can have a `Width` greater than two.

**Typical use:** Obtain the structure through `GlyphAt` to iterate over
a string while tracking both the byte offset and the screen column.

**Example:**

```cpp
#include "painter/tui/text.h"

int main()
{
    const painter::tui::Glyph Shape{};
    return Shape.Length == 1 && Shape.Width == 1 && !Shape.Odd ? 0 : 1;
}
```

### `GlyphAt`

```cpp
Glyph GlyphAt(std::string_view Text, std::size_t Byte,
              std::size_t Column = 0) noexcept;
```

**Summary:** Get the representation of a text element at a byte offset and current column.

**Parameters:**

- `Text` (`std::string_view`, in): input bytes; no null terminator is required.
- `Byte` (`std::size_t`, in): offset to the start of the element; must satisfy `Byte < Text.size()`.
- `Column` (`std::size_t`, in): current column used to calculate tab width; defaults to `0`.

**Returns:** A `Glyph` containing the input element's length, width, and glyphs.

**Side effects:** None; does not allocate memory.

**How it works:** Tabs are handled before UTF-8 decoding. For other bytes,
the function decodes one code point and passes it to
`BoxGlyphFoldCodepoint`. Invalid UTF-8 and control characters produce
a square with `Odd == true`; a valid unknown code point produces a square
without that flag.

**Notes:** The function does not check whether `Byte` is in bounds; it must
not be called on empty text. If `Byte` points into a multibyte sequence,
the continuation byte is treated as a separate error. `Column` affects
only tab handling.

**Typical use:** After each call, increment the byte offset by `Length`
and the column by `Width`; continue until the offset reaches the input size.

**Example:**

```cpp
#include "painter/tui/text.h"

int main()
{
    const auto Shape = painter::tui::GlyphAt("\u2260", 0);
    return Shape.Length == 3 && Shape.Width == 2 &&
           Shape.Cells[0] == '!' && Shape.Cells[1] == '=' ? 0 : 1;
}
```

### `CellsOf`

```cpp
std::size_t CellsOf(std::string_view Text) noexcept;
```

**Summary:** Calculate the width of text in cells, with columns counted from zero.

**Parameters:**

- `Text` (`std::string_view`, in): bytes to measure.

**Returns:** A `std::size_t` containing the sum of the element widths;
`0` for empty text.

**Side effects:** None; does not allocate memory.

**How it works:** The function processes the entire input using `GlyphAt`,
passing the accumulated width as `Column`. The byte offset increases by
`Length`, and the width increases by `Width`, so multibyte characters and
tabs are measured according to the same rules used by `PutText`.

**Notes:** The function measures a single sequence of cells even if the
input contains newline characters. Summing the widths does not check for
`std::size_t` overflow.

**Typical use:** Use it to align labels and calculate available space
before calling `PutText`. For strings containing tabs, the sum of
individual measurements may differ from the width of the concatenated string.

**Example:**

```cpp
#include "painter/tui/text.h"

int main()
{
    // A: 1, tab: 3, ≠: 2.
    return painter::tui::CellsOf("A\t\u2260") == 6 ? 0 : 1;
}
```

### `BytesWithin`

```cpp
std::size_t BytesWithin(std::string_view Text, std::size_t Cells) noexcept;
```

**Summary:** Find the byte length of the prefix that fits within a given number of cells.

**Parameters:**

- `Text` (`std::string_view`, in): original input bytes.
- `Cells` (`std::size_t`, in): permitted width in cells, starting at column `0`.

**Returns:** A `std::size_t` containing the number of prefix bytes, from
`0` through `Text.size()`; `0` for empty text or a zero-cell budget.

**Side effects:** None; does not allocate memory.

**How it works:** The function processes elements sequentially using
`GlyphAt`. If the next element would exceed the remaining width, iteration
stops before that element. A valid UTF-8 sequence, a two-cell replacement,
and a tab are never split.

**Notes:** Invalid UTF-8 is already treated as individual erroneous bytes,
so the function does not repair the input or guarantee that the result
is valid UTF-8. Adding the accumulated width to the next element's width
does not check for `std::size_t` overflow.

**Typical use:** Pass the result to `Text.substr(0, Bytes)` to truncate
without an ellipsis. If you need an indication of truncation, use
`Shortened`.

**Example:**

```cpp
#include "painter/tui/text.h"

int main()
{
    const std::string_view Text = "A\u2260B";
    // ≠ does not fit in the one remaining cell.
    const auto Bytes = painter::tui::BytesWithin(Text, 2);
    return Text.substr(0, Bytes) == "A" ? 0 : 1;
}
```

### `Shortened`

```cpp
std::string Shortened(std::string_view Text, std::size_t Width);
```

**Summary:** Obtain a copy of the text with an ellipsis if it exceeds the specified width.

**Parameters:**

- `Text` (`std::string_view`, in): original input bytes.
- `Width` (`std::size_t`, in): maximum result width in cells.

**Returns:** A `std::string` containing the original text if it fits;
otherwise, a prefix followed by three ASCII periods, or `Width` periods
when `Width <= 3`. The result is empty when `Width == 0`.

**Side effects:** Creates a string, whose storage may require memory
allocation. Exceptions from string operations propagate to the caller.

**How it works:** First, `CellsOf` measures the entire input. If truncation
is required and `Width > 3`, the function reserves three cells for `...`,
selects a prefix using `BytesWithin(Text, Width - 3)`, and appends the
periods. Because elements cannot be split, the result may occupy fewer
than `Width` cells; it is not padded with spaces.

**Notes:** The result preserves the original prefix bytes, including tabs
and invalid UTF-8; no glyph conversion takes place here. The `Width <= 3`
rule applies only when truncation is needed: for example,
`Shortened("A", 2)` returns `"A"`, not `".."`.

**Typical use:** For a label with a limited width, first call `Shortened`,
then pass the result to `PutText`.

**Example:**

```cpp
#include "painter/tui/text.h"

int main()
{
    const auto Label = painter::tui::Shortened("abcdef", 5);
    return Label == "ab..." ? 0 : 1;
}
```

### `PutText`

```cpp
std::int32_t PutText(Frame &Out, core::Point At, std::string_view Text,
                     std::int32_t Width, core::Color Ink, core::Color Under);
```

**Summary:** Write text into one frame row, limited by a cell-width budget.

**Parameters:**

- `Out` (`Frame &`, in-out): frame whose cells are modified.
- `At` (`core::Point`, in): starting column and row in frame coordinates.
- `Text` (`std::string_view`, in): input UTF-8 bytes.
- `Width` (`std::int32_t`, in): cell budget measured from `At.X`, not the ending column.
- `Ink` (`core::Color`, in): glyph color for elements without the `Odd` flag.
- `Under` (`core::Color`, in): background color of each cell written.

**Returns:** A `std::int32_t` containing the width of the processed prefix
before clipping to the frame boundaries. Returns `0` for empty text or
`Width <= 0`. This is not the number of cells successfully modified.

**Side effects:** Replaces cells in `Out` reached by the output. New cells
have `Visibility::Opaque` and `CellMark::None`. The background is replaced
with `Under`, and the color of erroneous elements with `OddInk`. The
remainder of the frame row is not cleared. The position of `Frame::Caret()`
is unchanged; no memory is allocated, and no output is presented on screen
by the function itself.

**How it works:** The local column starts at `0`; `GlyphAt` receives it
to calculate tab width. If the next element exceeds `Width`, that element
and all remaining text are skipped. Other elements are written through
`Frame::Put`, and tabs are expanded into spaces. The return value of
`Frame::Put` is ignored, so invisible cells outside the frame boundaries
still count toward the returned width.

**Notes:**

- A negative `At.X` clips the left portion of the output. If `At.Y` is
  outside the frame, the frame is not modified, but the return value is
  still not forced to zero.
- An entire two-cell element is checked against `Width`, but the frame's
  own boundary may clip just one of its cells.
- There is no ellipsis, wrapping, or background fill for unused cells.
  Before replacing a long label with a shorter one, clear its area with
  `Frame::Fill`.
- The function does not check for overflow in signed column arithmetic.
  Choose coordinates and a width budget so that adding the next element's
  width (up to four) to the local column, and adding `At.X`, both fit
  within `std::int32_t`.
- If multiple threads modify the same frame concurrently, the caller must
  provide synchronization; this function does not use locks.

**Typical use:** Measure a label with `CellsOf`, shorten it with `Shortened`
if necessary, then draw it using `PutText`. To display changes to the frame
of an open `Stage`, call `Stage::Present()` separately and handle its result.

**Example:** This standalone example creates a frame in memory; it does not open a display.

```cpp
#include "painter/tui/text.h"

int main()
{
    namespace tui = painter::tui;
    namespace core = painter::core;

    auto Created = tui::Frame::Create({12, 1});
    if (!Created) return 1;
    tui::Frame &Out = *Created;
    Out.Clear();

    // A occupies column 1, the tab occupies columns 2–4, != occupies 5–6.
    const auto Used = tui::PutText(Out, {1, 0}, "A\t\u2260", 6,
                                   core::Colors::White, core::Colors::Black);
    return Used == 6 && Out.Read({5, 0}).Glyph == '!' &&
           Out.Read({6, 0}).Glyph == '=' ? 0 : 1;
}
```

## Choosing a Writing Method

`Frame::Write` writes each byte as an already prepared glyph code and does
not perform UTF-8 decoding. Use it for strings of prepared glyph codes,
and use `PutText` for UTF-8. The result of `Shortened` remains UTF-8 text;
it must not be treated as a string of prepared glyph codes merely because
its width has been limited.

`PutText` modifies only the frame buffer. It does not open a surface,
receive key presses, or publish a layer to DisplayServer. Other Painter
modules handle those tasks.

## Usage in Applications

- [`wizard/paint.cpp`](../../../../src/userspace/utils/wizard/paint.cpp):
  `Tower::PaintHints` measures labels with `CellsOf`, selects hints that
  fit, and renders them with `PutText`; arrows are represented as UTF-8 bytes.
- [`wizard/speech.cpp`](../../../../src/userspace/utils/wizard/speech.cpp):
  `Wrapped` measures words with `CellsOf` and constructs lines itself;
  `Speech::Paint` renders them with `PutText`. Word wrapping is handled
  by the application.
- [`casino/desk.cpp`](../../../../src/userspace/utils/casino/desk.cpp):
  `Desk::PaintNotice` uses `Shortened` to limit a label's width.

The contracts were checked against source tree `bb6f5ac4` on 2026-11-13.
