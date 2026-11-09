#ifndef KIN_LOOK_H
#define KIN_LOOK_H

/* The look edit and read share, so the two are seen to be one family: their
 * colours, and a length as a person reads one. Text as cells, a window with a
 * frame and a shadow, and the box rules are Painter's (painter/tui/text.h,
 * window.h, glyph.h), the look every program's windows share.
 *
 * The colours are chosen for a framebuffer and checked against the sixteen of
 * a VGA text screen they are folded into there: the paper is black, the line
 * the caret or the reader is on dark grey, a selection dark blue, a match
 * brown, the match being looked at light yellow — still told apart where only
 * sixteen exist.
 */

#include <cstdint>
#include <string>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

namespace kin {

namespace core = painter::core;
namespace tui  = painter::tui;

inline constexpr core::Color Paper          = core::Color::Rgb(0x16, 0x16, 0x16);
inline constexpr core::Color LinePaper      = core::Color::Rgb(0x2C, 0x2C, 0x2C);
inline constexpr core::Color TextInk        = core::Color::Rgb(0xD4, 0xD4, 0xD4);
inline constexpr core::Color NumberInk      = core::Color::Rgb(0x5C, 0x5C, 0x5C);
inline constexpr core::Color CaretNumberInk = core::Color::Rgb(0xF0, 0xF0, 0xF0);
inline constexpr core::Color RuleInk        = core::Color::Rgb(0x48, 0x48, 0x48);
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

inline constexpr core::Color FieldPaper     = core::Color::Rgb(0x3A, 0x3A, 0x3A);
inline constexpr core::Color FieldInk       = core::Color::Rgb(0xF4, 0xF4, 0xF4);
inline constexpr core::Color ChosenPaper    = core::Color::Rgb(0x2E, 0x4A, 0x6A);

/* A length the way a person reads one: "12 B", "3 KB", "1.4 MB". */
std::string SizeText(std::uint64_t Bytes);

}  // namespace kin

#endif
