#ifndef DRAWCHECK_H
#define DRAWCHECK_H

/* drawcheck — what its parts share (drawcheck.cpp). */

#include <cstdint>

#include "painter/core/canvas.h"
#include "typecase/fonts.h"

namespace drawcheck {

/* A scene of every kind of thing the engine draws, the same for the same
 * Seed: shapes, strokes, gradients, pictures, masks, shadows, a clip, a canvas
 * laid over, a blur. */
void DrawScene(painter::core::Canvas &On, std::uint32_t Seed);

/* A scene of text, the same for the same Seed: lines of words of Latin and
 * Cyrillic, kerned, bold and leaning, underlined in an ink of their own,
 * wrapped, aligned, cut with an ellipsis, filled with colours and gradients,
 * some turned, in the system's fonts (Book). */
void DrawTextScene(painter::core::Canvas &On, std::uint32_t Seed, const typecase::Fonts &Book);

/* How long the engine's work takes the way in use, ns a pixel of a
 * 1920x1080 frame, said on one line. */
void SaySpeed();

/* How long setting a page of text takes, and drawing it — its glyphs kept,
 * and drawn new — said on one line. */
void SayTextSpeed(const typecase::Fonts &Book);

}  // namespace drawcheck

#endif  // DRAWCHECK_H
