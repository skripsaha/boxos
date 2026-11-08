#ifndef EDIT_PAINT_H
#define EDIT_PAINT_H

/* The look of edit: its colours and the few ways it draws — text cut to a
 * width, a name shortened with "...", a window with a frame and a shadow.
 *
 * The colours are chosen for a framebuffer and checked against the sixteen of
 * a VGA text screen they are folded into there: the paper is black, the
 * caret's line dark grey, a selection dark blue, a match brown, the match at
 * the caret light yellow — still told apart where only sixteen exist.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace edit {

namespace core = painter::core;
namespace tui  = painter::tui;

inline constexpr core::Color Paper          = core::Color::Rgb(0x16, 0x16, 0x16);
inline constexpr core::Color CaretLinePaper = core::Color::Rgb(0x2C, 0x2C, 0x2C);
inline constexpr core::Color TextInk        = core::Color::Rgb(0xD4, 0xD4, 0xD4);
inline constexpr core::Color NumberInk      = core::Color::Rgb(0x5C, 0x5C, 0x5C);
inline constexpr core::Color CaretNumberInk = core::Color::Rgb(0xF0, 0xF0, 0xF0);
inline constexpr core::Color RuleInk        = core::Color::Rgb(0x48, 0x48, 0x48);
inline constexpr core::Color OddInk         = core::Color::Rgb(0xE0, 0x9A, 0x30);
inline constexpr core::Color SelectedPaper  = core::Color::Rgb(0x26, 0x4F, 0x78);
inline constexpr core::Color MatchPaper     = core::Color::Rgb(0x5A, 0x44, 0x00);
inline constexpr core::Color HerePaper      = core::Color::Rgb(0xFF, 0xC8, 0x32);
inline constexpr core::Color HereInk        = core::Color::Rgb(0x14, 0x14, 0x14);
inline constexpr core::Color BlockPaper     = core::Color::Rgb(0xE8, 0xE8, 0xE8);
inline constexpr core::Color BlockInk       = core::Color::Rgb(0x14, 0x14, 0x14);

inline constexpr core::Color BarPaper       = core::Color::Rgb(0x1F, 0x1F, 0x1F);
inline constexpr core::Color BarInk         = core::Color::Rgb(0xA8, 0xA8, 0xA8);
inline constexpr core::Color ShownTabPaper  = core::Color::Rgb(0x3C, 0x3C, 0x3C);
inline constexpr core::Color ShownTabInk    = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color TabNumberInk   = core::Color::Rgb(0xFF, 0xB0, 0x20);
inline constexpr core::Color DirtyInk       = core::Color::Rgb(0xFF, 0xB0, 0x20);
inline constexpr core::Color SafetyPaper    = core::Color::Rgb(0x8C, 0x24, 0x24);
inline constexpr core::Color OvertypePaper  = core::Color::Rgb(0x22, 0x55, 0x88);
inline constexpr core::Color EverywherePaper = core::Color::Rgb(0x2A, 0x6A, 0x4A);
inline constexpr core::Color BadgeInk       = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color GoodInk        = core::Color::Rgb(0x7C, 0xD6, 0x8A);
inline constexpr core::Color TroubleInk     = core::Color::Rgb(0xFF, 0x6E, 0x6E);

inline constexpr core::Color WindowPaper    = core::Color::Rgb(0x24, 0x24, 0x24);
inline constexpr core::Color WindowRule     = core::Color::Rgb(0x9A, 0x9A, 0x9A);
inline constexpr core::Color WindowInk      = core::Color::Rgb(0xD4, 0xD4, 0xD4);
inline constexpr core::Color TitleInk       = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color HintInk        = core::Color::Rgb(0x8C, 0x8C, 0x8C);
inline constexpr core::Color FieldPaper     = core::Color::Rgb(0x3A, 0x3A, 0x3A);
inline constexpr core::Color FieldInk       = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color ChosenPaper    = core::Color::Rgb(0x2E, 0x4A, 0x6A);

/* Box-drawing glyphs of the console font. */
inline constexpr char RuleAcross = static_cast<char>(0xC4);
inline constexpr char RuleDown   = static_cast<char>(0xB3);
inline constexpr char CornerTL   = static_cast<char>(0xDA);
inline constexpr char CornerTR   = static_cast<char>(0xBF);
inline constexpr char CornerBL   = static_cast<char>(0xC0);
inline constexpr char CornerBR   = static_cast<char>(0xD9);
inline constexpr char TeeLeft    = static_cast<char>(0xC3);
inline constexpr char TeeRight   = static_cast<char>(0xB4);
inline constexpr char MoreLeft   = static_cast<char>(0xAE);   /* « a line goes on to the left */
inline constexpr char MoreRight  = static_cast<char>(0xAF);   /* » and to the right */

/* A length the way a person reads one: "12 B", "3 KB", "1.4 MB". */
std::string SizeText(std::uint64_t Bytes);
/* The cells Text takes on the glass. */
std::size_t CellsOf(std::string_view Text) noexcept;
/* Text in no more than Width cells: its end cut off for "..." when it is longer. */
std::string Shortened(std::string_view Text, std::size_t Width);
/* Text from At, no further than Width cells; the cells it took. */
std::int32_t PutText(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Under);

core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept;
/* A window over Area: its paper, a frame, Title in the top edge at the left
 * and Note at its right, Hint in the bottom edge, a shadow beneath. What is
 * inside the frame is returned. */
core::Rect DrawWindow(tui::Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note,
                      std::string_view Hint);
/* A line across a window's inside, joined to its frame. */
void DrawRule(tui::Frame &Out, core::Rect Area, std::int32_t Row);

}  // namespace edit

#endif
