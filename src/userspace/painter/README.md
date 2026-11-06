# Painter

Painter is the BoxOS family for user-facing drawing. It is one long-lived
home for related graphics APIs, with separate contracts for each kind of
surface:

- `painter::core` — geometry, rectangles, clipping, and other backend-neutral
  values that are genuinely shared;
- `painter::tui` — the first implementation: `Frame`, `Stage`, `Surface`, regions,
  opaque and transparent cells, compositing, glyphs, half-block pixel drawing,
  palettes, and typed console input;
- `painter::gui` — the future home for real pixel graphics, windows, paths,
  brushes, pointer input, and other GUI-specific contracts.

The GUI family belongs here from the beginning as an architectural boundary,
but it must not be represented by empty stubs. Until a real GUI backend and
contract exist, only `painter::tui` is implemented.

## Purpose

Painter hides the kernel display path from application authors. A program
should be able to open a `painter::tui::Stage`, draw a `Frame` using cells,
text, glyphs, rectangles, or two-pixel-high RGB primitives, present it, and
receive typed input without knowing about `TextCell`, `TouchTag`, Manifest,
`vga_paint`, or the kernel Canvas.

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
overlay frame whose untouched cells do not replace the destination. A
`Surface::Paint` is an opaque rectangular blit; `Surface::Composite` turns the
overlay into horizontal opaque runs and leaves transparent cells alone. The
kernel still receives its existing rectangular `TextCell` operations, so the
transparency contract remains a Painter concern and the display ABI stays
stable.

## Layering

```text
boxlib       C ABI and low-level BoxOS calls
boxcxx       BoxOS C++ runtime and native primitives
Painter      user-facing graphics family
apps/utils   programs and their domain models
```

Painter is a separate userspace library built above `boxcxx`; it is not a
second runtime and it does not replace `boxcxx` or `boxlib`.

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
`qr`, `passport`, `taskcenter`, `playtime`, `lavalamp`, `donut`, and `slime` link through
`libpainter.a`;
`qr`, `passport`, `donut`, and `slime` use the regional `Surface` contract.
The `tumbleweed` utility uses the full-screen `Stage` contract for its
retained desert frame.
Their models, animation,
persistence, layout algorithms, and visual composition remain application
code. The five older screen implementations in `apps` (`wordle`, `snake`,
`bigclock`, `calendar`, and `bonsai`) now also link through `libpainter.a` and
use Stage, Frame, and typed events. The Git checkpoint before implementation is tagged
`painter-preimplementation`, and the implementation branch proceeds in
bounded commits so that checkpoint remains an immediate rollback point.
