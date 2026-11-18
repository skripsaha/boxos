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
- `painter::gui` — a program's items on the screen (see "Items" below): an
  `Area` the display gives it, and `Changes` that say what the area shows —
  groups, texts, shapes, pictures and canvases, each with the properties it
  says — shown a step at a time and composed by the display.

## Purpose

Painter hides the display path from application authors. A program
should be able to open a `painter::tui::Stage`, draw a `Frame` using cells,
text, glyphs, rectangles, or two-pixel-high RGB primitives, present it, and
receive typed input without knowing about `ItemCell`, `TouchTag`, Manifest,
or the display's items language.

The first TUI implementation is synthesized from the proven utility patterns:

- `Stage` screen ownership and teardown;
- cell frame composition;
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
overlay frame whose untouched cells expose what lies below. A `Surface` keeps
cells of its own over the console, an area owned by its process: `Paint`
replaces a rectangle with opaque cells, while `Composite` replaces the selected
rectangle with the frame's coverage and clears transparent cells from it.
`Surface::Clear` empties all of it without publishing an intermediate erase
frame; the next draw publishes the replacement once. The kernel draws none of
it: the display composes the areas, so the transparency contract and z-order
are Painter's and the display's alone.

The caret is part of the picture. `Frame::SetCaret` places the one mark a cell
cannot draw — a line under the glyph where text goes — and a frame has none
until one is placed, so a picture without text input shows no caret at all. A
`Stage` shows the caret of the frame it presents; a `Surface` keeps the caret
of what is composited into it, and what replaces a rectangle replaces the
caret in it. Only `ConsoleSurface` has a cursor of its own: the point where the
console goes on once its cells are part of its text.

## Glass and finer edges

The display says what its glass is, and `Stage::Glass()` (also `Surface` and
`ConsoleSurface`, and `Frame::Glass()` for the frame a `Stage` owns) gives
`GlassKind::Text` for a text page of 16 colours or `GlassKind::Pixels` for a
framebuffer. Both draw the same cells; the kind says what a cell may look
like, so a program asks it and does not guess from the screen's size. Painter
does not draw pixels for it: `painter::tui` is cells, half-blocks and CP437
glyphs; real pixels are drawn on a `painter::core::Canvas`.

The display also says which grids finer than the halves its glass draws
exactly (`ITEM_MOSAIC_*` of `box/items.h`: quadrants, sextants, octants), and
`Mosaics()` of `Stage`, `Surface`, `ConsoleSurface` and `Frame` gives them.

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

## Items

A program that draws with pixels says what it shows, and the display keeps it
(`box/items.h`, `painter/gui/area.h`). `Area::Open` asks for an area — over
everything, the whole screen, or in the console's flow — and the display
decides where it lies and how large it is, telling its size in points and its
scale, pixels to a point. The program makes items in it, each named by a
number of its own: a group, a text, a shape, a picture, a canvas of its own
pixels. An item says only what differs from what its role gives it — a place
and size in points, opacity, a turn, a clip, corners, an outline, fills, a
stroke, shadows, a text's characters, style, coloured stretches and box — and
what it leaves unsaid the system's theme and fonts say: Inter for text,
JetBrains Mono for code, the screen's colours for ink and page.

`Changes` keeps what was said until `Area::Present` puts it in the area's ring:
the order of making and removing as it was said, and of each property only the
last word, so a full ring never makes a program wait — what did not fit goes
with the next step. The display applies a step whole or not at all, and tells
back in a second ring: the area's size and scale, an item sized by its content,
a change refused and why, a step shown and what its frame cost, a canvas page
free to draw in again. Pictures, canvases and fonts go in blocks of memory the
display makes for the program alone (`Area::OpenPicture`, `OpenCanvas`,
`OpenFont`).

The display composes only where something changed: what it keeps of each item
— a text laid, a picture of a shape at the screen's scale — is drawn again only
when the item's look changes, an opaque item hides what lies beneath it, a
group clips its items and is seen through as one. On a text page the texts are
written into the cells and the rest is shown by its caption.

A grid of cells is an item too (`Kind::Cells`): cells in the console's look,
each a character, its ink and paper, and a mark, with a caret of its own, said
as the runs of cells that changed. `painter::tui` is drawn with it: a `Stage`,
a `Surface` and a `ConsoleSurface` are each an area holding one grid as large
as the console and on its grid, so a program's frames go to the display as
the cells that changed and nothing in the programs differs. The console itself
is the lowest item on the glass; an area in its flow lies on its text and
moves with it when it scrolls, and `Area::Commit` folds an area's cells into
the console's text, as `ConsoleSurface::Commit` does.

### Items as objects (`painter/gui/items.h`)

A program writes its items as objects, made in an area or a group and told
what they show with `Set…` — words that chain:

```cpp
gui::Text Time = Screen->Text("12:46", gui::Role::Heading);
Time.SetPlace({16, 8});
gui::Shape Plate = Screen->Rectangle(gui::Role::Panel);
Plate.SetSize({200, 40}).SetCorners(painter::core::Corners::All(6));
Screen->Present();
Time.SetText("12:47");          // only this goes to the display
```

`Text`, `Rectangle`, `Ellipse`, `Outline`, `Group` (`Subgroup` in a group),
`Picture`, `Canvas` and `Cells` make them; `SetText`, `SetPlace`, `SetSize`,
`SetFill`, `SetStroke`, `SetShadow`, `SetCorners`, `SetOpacity`, `Show`, `Hide`
and the rest tell them. The numbers the language names items by are painter's.
An object keeps its item: moved, the item goes with it; destroyed or
`Remove()`d, the item is gone; an object outliving its area does nothing. What
was said of each property is kept, so a property told again as it was goes
nowhere — a program may tell an item all of itself every time.

### Frames described whole (`painter/gui/frame.h`)

Or a program describes each frame whole, and painter sends only what differs:

```cpp
Screen->Draw([&](gui::Frame &F) {
    gui::Group Rows = F.Group();
    for (const Row &Each : List) Rows.Text(Each.Name).Key(Each.Id).SetPlace(Each.At);
});
```

Each item is found again in the frame before by its parent, its kind and its
place among its siblings, or by the `Key` it says; what is new is made, what
changed said, what it no longer says unsaid, what is gone removed, what moved
among its siblings restacked — one step. A frame as the one before sends
nothing. A program that draws no frames carries none of this.

### Measuring, a canvas, waiting

`gui::Measure(Area, text, Role, width)` lays a text beforehand as the display
will — the same reading of the language, the same look of the roles and the
same laying out (`painter/items`), in the program's fonts (the system's, then
its own) — so a size known beforehand is the size the display lays; on a text
page a character is a cell.

A `Canvas` is the program's own pixels, real pixels of the screen: `Draw`
paints the next picture on the page the display does not show, waiting until
it lets one go, and copies the rest of the picture shown when only a part of
it changed.

`Area::Wait` waits for whatever comes next — what the display tells, a key of a
`painter::tui::Keyboard` given it, the strand's alarm (`box/alarm.h`) — with no
time out: a clock sets an alarm for the next second and waits.

### Things in time

Any change may take time, and the display plays it by the clock while the
program sleeps (`box/items.h` `ITEM_OP_ANIMATE`). Everything said inside
`Animate` goes in time — of objects and of frames described whole alike:

```cpp
Screen->Animate(gui::Animation::Out(300ms), [&] {
    Plate.SetOpacity(1).SetPlace({16, 40});
    Time.SetText("12:47");          // the old figures fade into the new
});
```

A property of numbers or colours goes through the values between — colours as
the eye sees them, through OKLab; one with none between, a text's characters, a
picture, a role, fades from its old look into the new; hidden fades out or in.
It goes from where it stands when it begins: said again on its way it goes on
from there, a spring keeping its speed; said plainly it is at once what was
said. An item never shown takes its first words at once — nothing flies in from
nowhere — and a property said plainly before an animation of it in its step is
where it starts, so a list fades in one by one with `Delay`. `Number` names an
animation: its end comes back as an `AnimationEnd` event, and another with
`After` set to it begins the moment it ends. Curves: `Linear`, `In`, `Out`,
`InOut`, `Steps`, `Spring` with its `Bounce`, a Bézier of one's own; `Repeats`
(or `Forever`) and `Back`.

The person decides how much moves: with less motion, things moved, sized,
turned or reshaped are at once where they go and fades are short, and every
animation plays at the person's speed (`settings display motion`). An area is
told both (`Area::LessMotion`, `MotionSpeed`), so what a program moves itself
moves as the person wants. On a text page every change is at once. A frame is
drawn only while something seen moves; a turn or a scale on its way lays the
picture already drawn, and draws it sharp again once still.

### For C (`painter/gui.h`)

The same areas and items for C, named plainly: `gui_open`, `gui_text`,
`gui_rectangle`, `gui_group`, `gui_set_text`, `gui_set_place`, `gui_set_fill`,
`gui_present`, `gui_wait`, `gui_measure`, `gui_close` — an item a number,
removed with `gui_remove` or with its area; `gui_animate` makes what follows go
in time, `GUI_EVENT_ANIMATION_END` tells its end, and `gui_less_motion` and
`gui_motion_speed` say how the person wants things to move. A C program that measures nothing
carries no reader of fonts.

## Layering

```text
boxlib       C ABI and low-level BoxOS calls
boxcxx       BoxOS C++ runtime and native primitives
DisplayServer userspace console transport and composition
Painter      user-facing graphics family and items client
apps/utils   programs and their domain models
```

`display.elf` is the single userspace DisplayServer. It owns the console lanes,
keyboard routing, process cleanup, and final composition. Painter is a client
library above `boxcxx`; it publishes programs' areas of items — painter::tui's
cells among them — to that server and does not draw through a second display
path. There is no separate daemon and compositor binary.

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

Time has its part of the engine too: `Easing` is how a change goes over the
time it takes — straight, slowly at first, slowing at the end, both, in jumps,
a Bézier of one's own — and `Spring` a weight on a spring, settling in the time
it is given, bouncing past its rest by as much as it is told, its speed handed
on when it is cut (`painter/core/easing.h`); `Color::Transition` walks a colour
to another as the eye sees it, through OKLab; `Canvas::FadeTo` fades one picture
into another pixel for pixel.

## Text

Typecase (`src/userspace/typecase`) sets text; Painter draws it. A program
asks for the system's fonts (`SystemFonts()`, `painter/core/system_fonts.h`:
every file on the volume tagged `font`, its default family the person's
`text font`) or reads its own (`typecase::Font::Read`), and gathers them in a
`typecase::Fonts` book: families by name, the face nearest a weight and a
slant found as CSS finds it, bold and leaning made where a family has no such
face, a character a face has not taken from the next family that has it, one
no font has drawn as its code — text never vanishes.

A `typecase::Text` is its characters, its `Style` and `Span`s that each say
only what they change — family, size, weight, italic, letter spacing, line
height, lines under and through, an ink — and whether it is `Secret`.
`typecase::Layout::Of(Text, TextBox, Fonts)` lays it in a box: kerned (GPOS
pairs under `kern`, by writing, or the kern table), broken between words, a
word too long between its characters, at a soft hyphen showing a hyphen,
aligned left, in the middle, right or justified, cut to its lines and height
with or without an ellipsis, or grown. The layout says how large it came to
and answers where the caret stands before a character, which character is
nearest a point, where a stretch lies, and the text to copy (never a secret's).

`Canvas::FillText` fills a layout with any `Fill`s, all of it or one ink: its
glyphs drawn once at their size and a quarter pixel's place and kept, one
cache for the program, a line's glyphs gathered into one coverage and laid
once; turned, slanted or larger than 256 pixels to the em it fills the text's
shape. `TextPath` is that shape — glyph outlines and decoration lines — to
stroke, cast a shadow or clip by. Not yet: ligatures, writings set right to
left, input methods.

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
