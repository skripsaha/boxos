#ifndef BOXOS_DISPLAY_AREAS_H
#define BOXOS_DISPLAY_AREAS_H

// Areas — the display's side of the items' language (box/items.h): every
// program's areas and blocks, the questions that open, fold and close them
// and the messages that step them, and the events told back. What they show
// is put on the glass by the frame (frame.h).
//
// Where an area lies is the display's to decide: over everything, as large as
// wished and in the middle of the screen; the whole screen; or in the
// console's flow, as wide as it, on rows the console gives it after the text
// so far, moving up with the text. An area of a program that lives behind is
// not shown (front.h), unless it is to be shown always. The memory of an area
// and of a block is made by the display for the asking program alone, under a
// name of the display's, reused.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "box/core/result.h"
#include "box/items.h"
#include "boxos_kctx.h"
#include "cells_draw.h"
#include "compositor.h"
#include "console_model.h"
#include "painter/core/geometry.h"

// The glass known: its pixels (a text page's eighths and sixteenths of a
// cell), whether it is a text page, the console's look (its cells, where
// they lie, the grids its characters are drawn exactly in), how large a point
// is drawn in pixels of 256 (0 for the display to choose by the screen), the
// theme items are drawn in, and who is told where an area that is gone was.
struct AreasGlass {
    painter::core::Rect Screen{};
    bool                Text    = false;
    std::uint8_t        Mosaics = 0;
    std::uint32_t       Scale   = 0;
    const CellsLook    *Cells   = nullptr;
    const items::Theme *Look    = nullptr;
    void (*Vacated)(const painter::core::Rect &Where) noexcept = nullptr;
};
void AreasStart(const AreasGlass &Glass);

// DISP_CMD_AREA_OPEN and DISP_CMD_BLOCK_OPEN, asked of this display (Asked) by
// the strand From names: answered here, a refusal too. An area in the flow
// takes its rows of Console. True when an area opened.
bool AreasOpen(const Result &Asked, const AskFrom &From, const DisplayAreaOpenRequest &Request, ConsoleModel &Console) noexcept;
void AreasBlockOpen(const Result &Asked, const AskFrom &From, const DisplayBlockOpenRequest &Request) noexcept;

// An area's cells taken to be folded into the console's text: where they lie
// on the console, Width x Height of them (a cell that shows nothing where
// none lies), the console's rows scrolled away first, the caret they leave,
// and what the fold says into the log ring — what the cells show, never the
// text beneath, which the ring holds already.
struct AreaFold {
    std::int32_t          X = 0, Y = 0;
    std::uint32_t         Width = 0, Height = 0;
    std::vector<ItemCell> Cells;
    std::uint32_t         ScrollRows = 0;
    std::uint32_t         CaretX = 0, CaretY = 0;
    bool                  Caret = false;
    std::string           Text;
};
// DISP_CMD_AREA_COMMIT by the strand (Pid, Generation): the area's steps
// read, its cells taken, and the area gone — or why not, and it stays.
std::expected<AreaFold, error_t> AreasCommit(std::uint32_t Pid, std::uint32_t Generation,
                                             const DisplayAreaCommitRequest &Request, const ConsoleModel &Console) noexcept;

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

// The areas in the flow laid where their rows of the console are now, the
// console's rows gone up off it counted RowsGone.
void AreasFlow(std::uint64_t RowsGone) noexcept;

// Every item drawn again in the theme as it now is.
void AreasTheme() noexcept;

// The areas, bottom to top: those in the console's flow, those of the whole
// screen, those over everything, those shown always — each by its age.
void AreasInOrder(std::vector<items::AreaState *> &Into);

// Whether anything of the areas changed since a frame last showed them.
bool AreasWant() noexcept;

// A frame showed the areas — composing took Micros and put Pixels on the
// glass: each area whose step it showed told so, and every bell rung that
// has news.
void AreasShown(std::uint32_t Micros, std::uint64_t Pixels) noexcept;

#endif
