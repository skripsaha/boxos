/* look.h — the game's colours: the day's and the night's sky behind it, and
 * what keeps its colour by day and by night — the pipes, the ground, the bird
 * and the boards. The night is when the machine's clock says. */
#ifndef FLAPPYBIRD_LOOK_H
#define FLAPPYBIRD_LOOK_H

#include "painter/core/color.h"

namespace flappybird {

using painter::core::Color;

struct Sky {
    Color Top, Horizon;    // the sky from its top down to the clouds
    Color Cloud, CloudFoot;
    Color City, Windows;
    Color Bush, BushEdge;
    Color Sand;            // the ground under its grass, a little dimmer at night
    bool  Stars = false;
};

inline constexpr Sky Day{Color::Rgb(0x46, 0xB6, 0xC6), Color::Rgb(0x9A, 0xE0, 0xE2), Color::Rgb(0xF4, 0xFC, 0xF2),
                         Color::Rgb(0xD5, 0xF0, 0xE2), Color::Rgb(0xC3, 0xE6, 0xC4), Color::Rgb(0x9C, 0xD3, 0xA8),
                         Color::Rgb(0x62, 0xD9, 0x6A), Color::Rgb(0x45, 0xB5, 0x52), Color::Rgb(0xDE, 0xD8, 0x95), false};
inline constexpr Sky Night{Color::Rgb(0x08, 0x16, 0x30), Color::Rgb(0x1E, 0x4F, 0x72), Color::Rgb(0x35, 0x62, 0x80),
                           Color::Rgb(0x27, 0x4E, 0x6A), Color::Rgb(0x1B, 0x38, 0x4E), Color::Rgb(0xF7, 0xD5, 0x6A),
                           Color::Rgb(0x2B, 0x7A, 0x4C), Color::Rgb(0x1F, 0x5E, 0x3A), Color::Rgb(0xC4, 0xBC, 0x7E), true};

namespace ink {
inline constexpr Color Outline    = Color::Rgb(0x54, 0x38, 0x47);   // every edge of the game
inline constexpr Color Grass      = Color::Rgb(0x73, 0xBF, 0x2E);
inline constexpr Color GrassLight = Color::Rgb(0x9C, 0xE6, 0x59);
inline constexpr Color GrassDark  = Color::Rgb(0x55, 0x80, 0x22);
inline constexpr Color PipeEdge   = Color::Rgb(0x5C, 0xA4, 0x24);
inline constexpr Color PipeLit    = Color::Rgb(0xA6, 0xEC, 0x62);
inline constexpr Color PipeBody   = Color::Rgb(0x78, 0xC5, 0x32);
inline constexpr Color PipeShade  = Color::Rgb(0x4B, 0x88, 0x1C);
inline constexpr Color Feather    = Color::Rgb(0xF9, 0xD4, 0x3A);
inline constexpr Color FeatherLit = Color::Rgb(0xFF, 0xF6, 0xA0);
inline constexpr Color FeatherDim = Color::Rgb(0xE3, 0x8E, 0x14);
inline constexpr Color Wing       = Color::Rgb(0xFF, 0xF7, 0xDC);
inline constexpr Color WingShade  = Color::Rgb(0xF2, 0xDA, 0x8A);
inline constexpr Color LipTop     = Color::Rgb(0xF8, 0x74, 0x2A);
inline constexpr Color LipBottom  = Color::Rgb(0xE8, 0x50, 0x22);
inline constexpr Color White      = Color::Rgb(0xFF, 0xFF, 0xFF);
inline constexpr Color Black      = Color::Rgb(0x00, 0x00, 0x00);
inline constexpr Color Title      = Color::Rgb(0xFC, 0xA2, 0x3C);
inline constexpr Color Board      = Color::Rgb(0xDE, 0xD8, 0x95);
inline constexpr Color BoardLine  = Color::Rgb(0xF6, 0xF2, 0xC8);
inline constexpr Color Label      = Color::Rgb(0xE6, 0x6A, 0x18);
inline constexpr Color New        = Color::Rgb(0xE5, 0x3B, 0x2B);
inline constexpr Color Hint       = Color::Rgb(0xFF, 0xFF, 0xFF);
}  // namespace ink

}  // namespace flappybird

#endif
