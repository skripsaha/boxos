/* look.h — the game's colours: a walnut board in a darker frame with a line
 * of brass, maple and walnut squares, ivory and ebony pieces, the table under
 * it all, and the marks laid on the squares. */
#ifndef CHESS_LOOK_H
#define CHESS_LOOK_H

#include "painter/core/color.h"

namespace chess::ink {

using painter::core::Color;

inline constexpr Color Table      = Color::Rgb(0x1C, 0x2B, 0x26);   // the felt round the board
inline constexpr Color TableEdge  = Color::Rgb(0x0A, 0x11, 0x0F);
inline constexpr Color Frame      = Color::Rgb(0x5A, 0x34, 0x1C);
inline constexpr Color FrameLit   = Color::Rgb(0x7A, 0x4A, 0x2A);
inline constexpr Color FrameDim   = Color::Rgb(0x3A, 0x20, 0x10);
inline constexpr Color Brass      = Color::Rgb(0xC9, 0xA2, 0x5C);
inline constexpr Color Label      = Color::Rgb(0xE9, 0xD3, 0xA6);
inline constexpr Color Maple      = Color::Rgb(0xF0, 0xDA, 0xB0);
inline constexpr Color MapleDim   = Color::Rgb(0xE0, 0xC4, 0x92);
inline constexpr Color Walnut     = Color::Rgb(0xB0, 0x7A, 0x4A);
inline constexpr Color WalnutDim  = Color::Rgb(0x8E, 0x5C, 0x36);

inline constexpr Color Ivory      = Color::Rgb(0xFF, 0xFC, 0xF2);
inline constexpr Color IvoryDim   = Color::Rgb(0xD9, 0xC9, 0xA9);
inline constexpr Color IvoryEdge  = Color::Rgb(0x3A, 0x2C, 0x22);
inline constexpr Color Ebony      = Color::Rgb(0x55, 0x4D, 0x48);
inline constexpr Color EbonyDim   = Color::Rgb(0x16, 0x13, 0x12);
inline constexpr Color EbonyEdge  = Color::Rgb(0x08, 0x07, 0x06);
inline constexpr Color EbonyLine  = Color::Rgb(0x9A, 0x90, 0x86);   // the lines drawn on an ebony piece

inline constexpr Color LastMove   = Color::Rgba(0xF6, 0xD3, 0x4A, 0x70);
inline constexpr Color Picked     = Color::Rgba(0x5C, 0xC8, 0x7A, 0x80);
inline constexpr Color Reach      = Color::Rgba(0x1E, 0x1A, 0x14, 0x52);   // a square the picked piece may go to
inline constexpr Color Check      = Color::Rgb(0xFF, 0x3B, 0x30);
inline constexpr Color Cursor     = Color::Rgb(0x6F, 0xD0, 0xFF);
inline constexpr Color White      = Color::Rgb(0xFF, 0xFF, 0xFF);
inline constexpr Color Shade      = Color::Rgba(0x00, 0x00, 0x00, 0x60);

}  // namespace chess::ink

#endif
