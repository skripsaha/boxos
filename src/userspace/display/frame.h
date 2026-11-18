#ifndef BOXOS_DISPLAY_FRAME_H
#define BOXOS_DISPLAY_FRAME_H

// The frame — what the glass shows, put together only where it changed (the
// owner's design, parts 6, 7 and 13): the console, the lowest item on the
// glass — its cells (console_model.h) in the console's look — and over it the
// programs' areas (areas.h), bottom to top, one way onto the glass for all of
// it.
//
// On a framebuffer the compositor (compositor.h) composes what changed, tile
// by tile on one canvas of a tile's size, from every item's drawing, each tile
// put into the mirror of the glass (mirror.h); where nothing an area draws
// lies over the console, its cells are written into the mirror straight. Of
// what the console changed only the cells the mirror does not show already
// are drawn. Where the console scrolled the mirror moves instead of being
// drawn again — all but what an area lays opaque over it, which stays still —
// and only the rows that came in, the caret, the areas over the text — where
// they lay and where the text under them went — and the stripe above what
// stayed still are composed. One caret is on the glass: the topmost shown
// cells item's that shows one, else the console's.
//
// A text page has no pixels: each frame its cells are composed whole — the
// console's, each area's cells over them, its texts written in, the rest by
// its caption.
//
// What goes onto the glass, and through what, is the easel's (easel.h).

#include <cstdint>
#include <memory>
#include <span>

#include "cell_grid.h"
#include "cells_draw.h"
#include "compositor.h"
#include "console_model.h"
#include "item_look.h"
#include "mirror.h"

class Typesetter;

// The glass: a text page of Cols x Rows cells, or a framebuffer of Width x
// Height pixels whose cells the typesetter pictures; and the screen's own
// colours, two triples.
struct FrameGlass {
    bool          Text = false;
    std::uint32_t Cols = 0, Rows = 0;
    std::uint32_t Width = 0, Height = 0;
    Typesetter   *Type = nullptr;
    std::uint32_t TextColour = COLOR_LIGHT_GRAY, PageColour = COLOR_BLACK;
};

// The glass known. The console's grid made, as many cells as the glass
// holds, and handed back for the console to write. Throws std::bad_alloc
// only.
std::shared_ptr<CellGrid> FrameStart(const FrameGlass &Glass);

// The look every item is drawn in: the theme, and the console's look of
// cells; and the glass's pixels (a text page's eighths and sixteenths of a
// cell).
const items::Theme  &FrameTheme() noexcept;
const CellsLook     &FrameCells() noexcept;
painter::core::Rect  FrameScreen() noexcept;
// The screen's text and page colours, two triples: the theme made from them,
// and the console drawn again in them (the areas' items: areas.h AreasTheme).
void FrameColours(std::uint32_t Text, std::uint32_t Page);

// Pixels to be composed again: something that lay there went.
void FrameVacated(const painter::core::Rect &Where) noexcept;
// Everything to be composed again at the next frame.
void FrameEverything() noexcept;

// Whether a frame has anything to show — the console's caret taken where it
// stands now first; Areas, whether anything of the areas changed.
bool FrameDue(const ConsoleModel &Console, bool Areas) noexcept;

// A framebuffer's frame: what changed composed into Into, Programs the areas
// bottom to top (areas.h AreasInOrder) — and what composing took, and how
// many pixels it put.
struct FrameCost {
    std::uint32_t Micros = 0;
    std::uint64_t Pixels = 0;
};
FrameCost FrameCompose(Mirror &Into, std::span<items::AreaState *const> Programs) noexcept;

// A text page's frame: its cells composed whole, and the caret into At.
std::span<const ItemCell> FramePage(std::span<items::AreaState *const> Programs, Caret &At) noexcept;

#endif
