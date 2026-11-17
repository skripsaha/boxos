# Painter

Painter is the BoxOS family for user-facing drawing. It is one long-lived
home for related graphics APIs, with separate contracts for each kind of
surface:

- `painter::core` — geometry and colours, and the drawing engine every pixel
  picture of the system is drawn with: a `Canvas` of sixteen-bit light,
  `Path`s filled and stroked, `Gradient`s, `Picture`s, `Mask`s, `Shadow`s,
  clips and blur (see "The drawing engine" below);
- `painter::tui` — the first implementation: `Frame`, `Stage`, `Surface`, regions,
  opaque and transparent cells, compositing, glyphs, half-block pixel drawing,
  palettes, and typed console input;
- `painter::gui` — the future home for real pixel graphics, windows, paths,
  brushes, pointer input, and other GUI-specific contracts.

The GUI family belongs here from the beginning as an architectural boundary,
but it must not be represented by empty stubs. Until a real GUI backend and
contract exist, only `painter::tui` is implemented.

## Purpose

Painter hides the display path from application authors. A program
should be able to open a `painter::tui::Stage`, draw a `Frame` using cells,
text, glyphs, rectangles, or two-pixel-high RGB primitives, present it, and
receive typed input without knowing about `TextCell`, `TouchTag`, Manifest,
or the display's layer protocol.

The first TUI implementation is synthesized from the proven utility patterns:

- `Stage` screen ownership and teardown;
- `TextCell` frame composition;
- CP437 block and box glyphs;
- RGB-to-half-block pixel mapping;
- clipping, rectangles, text, palettes, and sprites;
- explicit full-screen and regional painting contracts;
- transparent overlays that retain rectangular storage while exposing arbitrary
  visible shapes;
- Touch-backed key events and honest presentation errors.

The application keeps its own model, animation, simulation, persistence, and
TagFS policy. Painter is a drawing and input surface, not a storage layer,
command parser, game engine, widget tree, or TagFS abstraction.

`Frame::Clear` creates an opaque frame and is the right operation for a full
screen or a complete rectangular panel. `Frame::ClearTransparent` creates an
overlay frame whose untouched cells expose the layer below. A `Surface` keeps
one retained layer owned by its process: `Paint` replaces a rectangle with
opaque cells, while `Composite` replaces the selected rectangle with the frame's
coverage and clears transparent cells from that layer. `Surface::Clear` removes
the whole retained layer without publishing an intermediate erase frame; the
next draw publishes the replacement once. The kernel draws none of it: the
display composes the layers, so the transparency contract and z-order are
Painter's and the display's alone.

The caret is part of the picture. `Frame::SetCaret` places the one mark a cell
cannot draw — a line under the glyph where text goes — and a frame has none
until one is placed, so a picture without text input shows no caret at all. A
`Stage` shows the caret of the frame it presents; a `Surface` keeps the caret
of what is composited into it, and what replaces a rectangle replaces the
caret in it. Only `ConsoleSurface` has a cursor of its own: the point where the
console goes on once the layer is part of its text.

## Glass and finer edges

The display says what its glass is, and `Stage::Glass()` (also `Surface` and
`ConsoleSurface`, and `Frame::Glass()` for the frame a `Stage` owns) gives
`GlassKind::Text` for a text page of 16 colours or `GlassKind::Pixels` for a
framebuffer. Both draw the same cells; the kind says what a cell may look
like, so a program asks it and does not guess from the screen's size. Painter
does not draw pixels for it: `painter::tui` is cells, half-blocks and CP437
glyphs; real pixels are drawn on a `painter::core::Canvas`.

The display also says which grids finer than the halves its glass draws
exactly (`DisplayMosaic`: quadrants, sextants, octants), and `Mosaics()` of
`Stage`, `Surface`, `ConsoleSurface` and `Frame` gives them; today it says
none.

`PutPixel` has one pixel across a cell and two down. `FineCanvas`
(`painter/tui/fine_canvas.h`) is the opt-in finer way to draw shapes on a
`Frame`: each cell is two sub-pixels across and, down, as many as the glass
draws exactly (four, three, or two), and a shape (`Disc`, `Ellipse`, `Ring`,
`Rounded`, `Line`, `Polygon`, `Fill`, or `Cover` for any shape given as a
distance) is placed in `PutPixel`'s units and blended into each sub-pixel by
how much of it the shape covers. Every cell a shape reaches is then settled
by `ResolveMosaic` (`painter/tui/mosaic.h`). On a pixel glass that draws the
grid it is exact: the two colours and the one character of `boxos_mosaic.h`
that split the sub-pixels with the least colour error. Otherwise it picks the
glyph and the two colours that reproduce the four sub-pixels of two rows
with the least colour error:

- one colour (a space), the upper half, or the left half, of two mean colours;
- on pixels also the shades, by the font's own picture of them: a diagonal of
  two colours is the half shade, and one sub-pixel against three is dust of
  its colour over the others. A shade is a texture, so its error is raised
  by how far apart its two colours lie, and it is chosen only where a split
  cannot say the cell as well;
- on a text page only the first, for sixteen colours turn a dither into noise.

A canvas keeps no state: a cell the shape misses is not touched, one it
reaches becomes opaque, and shapes drawn over one another are laid in order.
`FineCanvas` and `PutPixel` coexist on one frame, and an existing program is
unchanged.

## Layering

```text
boxlib       C ABI and low-level BoxOS calls
boxcxx       BoxOS C++ runtime and native primitives
DisplayServer userspace console transport and composition
Painter      user-facing graphics family and layer client
apps/utils   programs and their domain models
```

`display.elf` is the single userspace DisplayServer. It owns the console lanes,
keyboard routing, process cleanup, and final composition. Painter is a client
library above `boxcxx`; it publishes retained scene/overlay layers to that
server and does not draw through a second display path. There is no separate
daemon and compositor binary.

## TUI plan

1. Extract the existing `Stage` behavior into the public TUI contract without
   changing current utility frames or input behavior.
2. Add a safe `Frame` surface with geometry types, clipping, text, glyphs,
   rectangles, borders, and color policy.
3. Add pixel operations that compose directly into a frame, preserving the
   Aquarium, Flame, Fireworks, Mandelbrot, Nyancat, Eyes, and QR techniques.
4. Add typed input, full-screen ownership, regional painting, and explicit
   lifetime/error rules.
5. Migrate existing utilities incrementally and compare their generated
   frames, colors, glyphs, timing, input, and teardown behavior before and
   after each migration.
6. Add higher-level controls only when several real applications require the
   same contract. Do not import an ncurses window/panel model by default.

## The drawing engine

`painter::core` draws pixels for every hand that draws them: the display's
composition, a program's own picture, `painter::gui`. A `Canvas` keeps each
pixel as four sixteen-bit numbers — red, green and blue light already times
its opacity, and the opacity (`Pixel`) — so a colour is laid over another as
light, the one way the system mixes (`boxos_color.h`), with room enough that a
wide dark gradient keeps no bands. On it:

- `Fill` and `Stroke` a `Path` — lines, curves, SVG arcs and path data, and
  rectangles, rounded rectangles of four radii and ellipses kept as
  themselves — under a `Transform`, by `FillRule::NonZero` or `EvenOdd`; a
  stroke with every `LineCap`, `LineJoin`, the miter limit, dashes, and
  `StrokeAlign` across, within or without its shape;
- with several fills at once, each a colour, a `Gradient` (linear, radial from
  a focal point, conic; `Extend` pad, repeat, reflect, none) or a `Picture`
  (smooth from the copy nearest its size, or sharp);
- `FillMask` a `Mask` — a glyph, a clip, a `Shadow` made once and kept, raised
  or pressed, blurred and spread — and `Draw` another canvas over it;
- `PushClip` a rectangle or a path, `Blur` an area as frosted glass, and
  `CopyOut` to the glass, dithered by a fixed pattern of blue noise so a pixel
  that does not change is written the same.

Every edge is covered as the brush covers it — one text, `boxos_shape.h`,
`boxos_stroke.h` and `boxos_paint.h`, for the brush in the loader, Typecase's
glyphs and this engine — and its rows are worked plain, with SSE2 or with AVX2
(`CpuPath`), the fastest the processor offers, every way the same bits.
`tools/draw_stand.sh` holds it on the host, `drawcheck` on the machine.

## Future GUI boundary

Future GUI work uses the same Painter family and shared core values where the
semantics are truly common. GUI surfaces, raster formats, alpha composition,
paths, brushes, windows, pointer input, and presentation guarantees remain in
`painter::gui`; they are not forced into the TUI cell model.

## Current state

Design accepted on 2026-11-05. The first implementation slice now provides
`painter::core` geometry and colors plus `painter::tui::Frame`,
`painter::tui::Stage`, and the regional `painter::tui::Surface`. All direct
legacy-`stage.h` consumers are now migrated:
`nyancat`, `fireworks`, `2048`, `tetris`, `minesweeper`, `mandelbrot`,
`aquarium`, `eyes`, `typer`, `tagmap`, `todo`, `flame`, `stringview`, `hexer`,
`qr`, `passport`, `taskboard`, `playtime`, `lavalamp`, `donut`, and `slime` link through
`libpainter.a`;
`qr`, `passport`, `donut`, and `slime` use the regional `Surface` contract.
The `tumbleweed` utility uses the full-screen `Stage` contract for its
retained desert frame.
Their models, animation,
persistence, layout algorithms, and visual composition remain application
code. The older screen implementations in `apps` (`wordle`, `snake`,
`bigclock`, `calendar`, `bonsai`, `hacker`, and `pipes`) now also link through
`libpainter.a` and use Stage, Frame, and typed events. The Git checkpoint before implementation is tagged
`painter-preimplementation`, and the implementation branch proceeds in
bounded commits so that checkpoint remains an immediate rollback point.
