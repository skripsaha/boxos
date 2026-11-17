#ifndef DRAWCHECK_H
#define DRAWCHECK_H

/* drawcheck — what its parts share (drawcheck.cpp). */

#include <cstdint>

#include "painter/core/canvas.h"

namespace drawcheck {

/* A scene of every kind of thing the engine draws, the same for the same
 * Seed: shapes, strokes, gradients, pictures, masks, shadows, a clip, a canvas
 * laid over, a blur. */
void DrawScene(painter::core::Canvas &On, std::uint32_t Seed);

/* How long the engine's work takes the way in use, ns a pixel of a
 * 1920x1080 frame, said on one line. */
void SaySpeed();

}  // namespace drawcheck

#endif  // DRAWCHECK_H
