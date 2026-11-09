#ifndef PAINTER_TUI_WINDOW_H
#define PAINTER_TUI_WINDOW_H

/* A window drawn over a frame, the one look every program's windows share:
 * its paper, a frame of box rules, a title in its top edge and a note at the
 * right of it, a hint in its bottom edge, and a shadow beneath that lifts it
 * off what it lies over. edit's and read's windows are these, and so are the
 * hub's. */

#include <cstdint>
#include <string_view>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace painter::tui {

inline constexpr core::Color WindowPaper = core::Color::Rgb(0x24, 0x24, 0x24);
inline constexpr core::Color WindowRule  = core::Color::Rgb(0x9A, 0x9A, 0x9A);
inline constexpr core::Color WindowInk   = core::Color::Rgb(0xD4, 0xD4, 0xD4);
inline constexpr core::Color TitleInk    = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color HintInk     = core::Color::Rgb(0x8C, 0x8C, 0x8C);
inline constexpr core::Color ChosenPaper = core::Color::Rgb(0x2E, 0x4A, 0x6A);   /* the row marked in a list */

/* A Width by Height area in the middle of Screen, no larger than it. */
core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept;
/* A window over Area: its paper, a frame, Title in the top edge at the left
 * and Note at its right, Hint in the bottom edge, a shadow beneath. What is
 * inside the frame is returned. */
core::Rect DrawWindow(Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note,
                      std::string_view Hint);
/* A line across a window's inside, joined to its frame. */
void       DrawRule(Frame &Out, core::Rect Inside, std::int32_t Row);

}  // namespace painter::tui

#endif  // PAINTER_TUI_WINDOW_H
