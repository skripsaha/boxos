/* backdrop.h — what lies behind and around the gallows, pixel by pixel: the sky
 * with its stars, the far mountains and the hill the gallows stands on, then the
 * round things drawn over them: the sun, clouds, the tree, the grass in the
 * wind and the birds. Wind is a number round 1 that rises and falls. */
#ifndef HANGMAN_BACKDROP_H
#define HANGMAN_BACKDROP_H

#include <cstdint>

#include "painter/tui/fine_canvas.h"
#include "painter/tui/frame.h"

#include "gallows.h"
#include "tones.h"
#include "view.h"

namespace hangman {

// The tree's trunk and how far up its crown is, for the leaves that fall from it.
inline constexpr float TreeAt = 13.0f;
inline constexpr float CrownUp = 30.0f;

// How far into dusk the first stars show; until then the backdrop does not change with the time.
inline constexpr float StarsFrom = 0.45f;

// The height of the hill at X, in the scene's units, level where the gallows stands.
float HillAt(float X, const Timbers &Wood);

float WindAt(std::uint64_t Now);

void PaintBackdrop(painter::tui::Frame &Out, const View &Where, const Timbers &Wood, const Tones &Tone, float Dusk,
                   std::uint64_t Now);
void PaintSun(painter::tui::FineCanvas &Fine, const View &Where, const Tones &Tone, float Dusk, float Joy, std::uint64_t Now);
void PaintClouds(painter::tui::FineCanvas &Fine, const View &Where, const Tones &Tone, std::uint64_t Now);
void PaintBirds(painter::tui::FineCanvas &Fine, const View &Where, const Tones &Tone, float Joy, std::uint64_t Now);
void PaintTree(painter::tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now);
void PaintGrass(painter::tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now);

}  // namespace hangman

#endif
