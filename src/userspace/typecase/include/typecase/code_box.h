#ifndef TYPECASE_CODE_BOX_H
#define TYPECASE_CODE_BOX_H

/* The picture of a character no source has: the cell filled, the
 * character's code cut out of it in small hexadecimal digits — four for a
 * character of the Basic Multilingual Plane, six beyond it — so that which
 * one is missing can be read off the screen. Where the digits do not fit
 * the cell, it is an empty box instead — a light line a pixel in from the
 * cell's edges, so missing characters side by side stay apart rather than
 * run together into a bar; a cell too small for even that is filled. */

#include "typecase/glyph_image.h"
#include "typecase/size.h"

namespace typecase {

GlyphImage DrawCodeBox(char32_t Character, const Size &At);

/* The empty box alone: what a missing character is when its code is not to
 * be said — the light line a pixel in from the cell's edges, or the cell
 * filled when it is too small for that. */
GlyphImage DrawEmptyBox(const Size &At);

}  // namespace typecase

#endif  // TYPECASE_CODE_BOX_H
