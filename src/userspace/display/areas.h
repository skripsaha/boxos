#ifndef BOXOS_DISPLAY_AREAS_H
#define BOXOS_DISPLAY_AREAS_H

// Areas — the display's side of the items' language (box/items.h): every
// program's areas and blocks, the questions that open them and the messages
// that step and close them, the events told back, and each frame of what they
// show composed over the console onto the glass (compositor.h).
//
// Where an area lies is the display's to decide: over everything, as large as
// wished and in the middle of the screen; the whole screen; or in the
// console's flow, as wide as it, from the row its caret stands on when it
// opens. An area of a program that lives behind is not shown (front.h). The
// memory of an area and of a block is made by the display for the asking
// program alone, under a name of the display's, reused as layers' are.
//
// On a framebuffer a frame composes only what changed, tile by tile on one
// canvas of a tile's size: the console's cells beneath (the easel draws them,
// unless an opaque item hides them), the areas' items over them, put on the
// glass. A text page has no pixels: each shown area's texts are written into
// the cells, and what can only be seen — a shape, a picture — by its caption.

#include <cstdint>
#include <span>
#include <vector>

#include "box/core/result.h"
#include "box/items.h"
#include "boxos_kctx.h"
#include "easel.h"
#include "painter/core/geometry.h"

// The glass known: the easel's pixels or cells, and how large a point is
// drawn, in pixels of 256 — 0 for the display to choose by the screen.
void AreasStart(const Easel &On, std::uint32_t Scale);

// DISP_CMD_AREA_OPEN and DISP_CMD_BLOCK_OPEN, asked of this display (Asked) by
// the strand From names: answered here, a refusal too. CaretRow is the
// console's caret's row, where an area in the flow begins. True when an area
// opened.
bool AreasOpen(const Result &Asked, const AskFrom &From, const DisplayAreaOpenRequest &Request, std::uint32_t CaretRow) noexcept;
void AreasBlockOpen(const Result &Asked, const AskFrom &From, const DisplayBlockOpenRequest &Request) noexcept;

// DISP_CMD_AREA_STEP, _CLOSE and _BLOCK_CLOSE from the strand (Sender,
// Generation).
void AreasStep(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Area) noexcept;
void AreasClose(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Area) noexcept;
void AreasBlockClose(std::uint32_t Sender, std::uint32_t Generation, std::uint32_t Block) noexcept;

// A death: the areas the strand (Pid, Generation) spoke for, and everything
// the program it founded held, gone.
void AreasDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// Which areas are shown, judged again: a program went behind or came
// forward. True when any changed.
bool AreasSeen() noexcept;

// The screen's text and page colours, the theme's ground: every item drawn
// again in them.
void AreasTheme(std::uint32_t Text, std::uint32_t Page) noexcept;

// Where the shown areas lie on a framebuffer, and pixels beneath them the
// console changed: what Present leaves to the frame (easel.h Over).
std::span<const painter::core::Rect> AreasCovered() noexcept;
void AreasDamage(const painter::core::Rect &Pixels) noexcept;

// Whether a frame has anything to do for the areas.
bool AreasWant() noexcept;

// A framebuffer's frame: what changed composed and put on the glass, and
// each area whose steps it shows told so, with what it cost.
void AreasFrame(Easel &On) noexcept;

// A text page's frame: each shown area's items written into the Cols x Rows
// composed cells.
void AreasCells(std::vector<GlassCell> &Cells, std::uint32_t Cols, std::uint32_t Rows) noexcept;

#endif
