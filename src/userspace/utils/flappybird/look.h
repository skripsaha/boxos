/* look.h — the game's colours: the day's and the night's sky behind it, and
 * what keeps its colour by day and by night — the pipes, the ground, the bird
 * and the boards. The night is when the machine's clock says. */
#ifndef FLAPPYBIRD_LOOK_H
#define FLAPPYBIRD_LOOK_H

#include "painter/core/color.h"

namespace flappybird {

using painter::core::Color;

struct Sky {
    Color Top, Horizon;                  // the sky from its top down to the clouds
    Color CloudFar, Cloud, CloudShade;   // the far bank; the near clouds lit and in their shade
    Color City, Windows;
    Color Trunk, Crown, CrownLit, CrownLight;   // the trees: their crowns in shade, lit and catching the sun
    Color Bush, BushLit, BushLight;
    Color Sand;                          // the ground under its grass, a little dimmer at night
    bool  Stars = false;
};

inline constexpr Sky Day{.Top        = Color::Rgb(0x46, 0xB6, 0xC6),
                         .Horizon    = Color::Rgb(0x9A, 0xE0, 0xE2),
                         .CloudFar   = Color::Rgb(0xBC, 0xE8, 0xE2),
                         .Cloud      = Color::Rgb(0xF6, 0xFD, 0xF4),
                         .CloudShade = Color::Rgb(0xCF, 0xEC, 0xE4),
                         .City       = Color::Rgb(0xC3, 0xE6, 0xC4),
                         .Windows    = Color::Rgb(0x9C, 0xD3, 0xA8),
                         .Trunk      = Color::Rgb(0x6F, 0x5A, 0x3E),
                         .Crown      = Color::Rgb(0x2F, 0x8A, 0x4E),
                         .CrownLit   = Color::Rgb(0x45, 0xA6, 0x5C),
                         .CrownLight = Color::Rgb(0x64, 0xC0, 0x70),
                         .Bush       = Color::Rgb(0x3A, 0xA6, 0x4C),
                         .BushLit    = Color::Rgb(0x5A, 0xCB, 0x62),
                         .BushLight  = Color::Rgb(0x8B, 0xE5, 0x7A),
                         .Sand       = Color::Rgb(0xDE, 0xD8, 0x95),
                         .Stars      = false};
inline constexpr Sky Night{.Top        = Color::Rgb(0x08, 0x16, 0x30),
                           .Horizon    = Color::Rgb(0x1E, 0x4F, 0x72),
                           .CloudFar   = Color::Rgb(0x28, 0x54, 0x73),
                           .Cloud      = Color::Rgb(0x3E, 0x6A, 0x88),
                           .CloudShade = Color::Rgb(0x2A, 0x52, 0x70),
                           .City       = Color::Rgb(0x1B, 0x38, 0x4E),
                           .Windows    = Color::Rgb(0xF7, 0xD5, 0x6A),
                           .Trunk      = Color::Rgb(0x2B, 0x2A, 0x24),
                           .Crown      = Color::Rgb(0x17, 0x3F, 0x2C),
                           .CrownLit   = Color::Rgb(0x1F, 0x56, 0x38),
                           .CrownLight = Color::Rgb(0x2A, 0x6B, 0x45),
                           .Bush       = Color::Rgb(0x1A, 0x4F, 0x33),
                           .BushLit    = Color::Rgb(0x27, 0x6F, 0x45),
                           .BushLight  = Color::Rgb(0x35, 0x8A, 0x55),
                           .Sand       = Color::Rgb(0xC4, 0xBC, 0x7E),
                           .Stars      = true};

namespace ink {
inline constexpr Color Outline    = Color::Rgb(0x54, 0x38, 0x47);   // every edge of the game
inline constexpr Color Grass      = Color::Rgb(0x73, 0xBF, 0x2E);
inline constexpr Color GrassLight = Color::Rgb(0x9C, 0xE6, 0x59);
inline constexpr Color GrassDark  = Color::Rgb(0x55, 0x80, 0x22);
inline constexpr Color PipeEdge   = Color::Rgb(0x5C, 0xA4, 0x24);
inline constexpr Color PipeLit    = Color::Rgb(0xA6, 0xEC, 0x62);
inline constexpr Color PipeBody   = Color::Rgb(0x78, 0xC5, 0x32);
inline constexpr Color PipeShade  = Color::Rgb(0x4B, 0x88, 0x1C);
inline constexpr Color Plume      = Color::Rgb(0x2B, 0xB3, 0xA8);   // the bird
inline constexpr Color PlumeLit   = Color::Rgb(0x8A, 0xEA, 0xDC);
inline constexpr Color PlumeDim   = Color::Rgb(0x17, 0x76, 0x74);
inline constexpr Color Breast     = Color::Rgb(0xFF, 0xF1, 0xD2);
inline constexpr Color BreastDim  = Color::Rgb(0xF4, 0xD2, 0x96);
inline constexpr Color Beak       = Color::Rgb(0xFF, 0xB8, 0x30);
inline constexpr Color BeakDim    = Color::Rgb(0xE0, 0x76, 0x10);
inline constexpr Color Blush      = Color::Rgb(0xFF, 0x86, 0x7C);
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
