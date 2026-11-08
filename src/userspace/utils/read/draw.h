#ifndef READ_DRAW_H
#define READ_DRAW_H

/* The few ways read draws beside the page itself: text cut to a width, a
 * name shortened with "...", a window with a frame and a shadow, in the
 * colours of the theme the page is in. */

#include <cstdint>
#include <string>
#include <string_view>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "theme.h"

namespace read {

namespace tui = painter::tui;

/* Box-drawing glyphs of the console font. */
inline constexpr char RuleAcross  = static_cast<char>(0xC4);
inline constexpr char DoubleRule  = static_cast<char>(0xCD);
inline constexpr char RuleDown    = static_cast<char>(0xB3);
inline constexpr char CornerTL    = static_cast<char>(0xDA);
inline constexpr char CornerTR    = static_cast<char>(0xBF);
inline constexpr char CornerBL    = static_cast<char>(0xC0);
inline constexpr char CornerBR    = static_cast<char>(0xD9);
inline constexpr char GoesOn      = static_cast<char>(0xAF);   /* » a line goes on past the edge */
inline constexpr char BookmarkSign = static_cast<char>(0x10);  /* ► */

/* Text from At, no further than Width cells; the cells it took. */
std::int32_t PutText(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Paper);
/* Text in no more than Width cells: its end cut off for "..." when it is longer. */
std::string  Shortened(std::string_view Text, std::size_t Width);
core::Rect   Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept;
/* A window over Area: its paper, a frame, Title in the top edge, Hint in the
 * bottom one, a shadow beneath. What is inside the frame is returned. */
core::Rect   DrawWindow(tui::Frame &Out, core::Rect Area, const Theme &Look, std::string_view Title,
                        std::string_view Hint);

}  // namespace read

#endif
