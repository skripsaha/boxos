#ifndef TYPECASE_CODE_BOX_H
#define TYPECASE_CODE_BOX_H

/* The picture of a character no source has: the cell filled, the
 * character's code cut out of it in small hexadecimal digits — four for a
 * character of the Basic Multilingual Plane, six beyond it — so that which
 * one is missing can be read off the screen. Where the digits do not fit
 * the cell, the cell is only filled. */

#include "typecase/glyph_image.h"
#include "typecase/size.h"

namespace typecase {

GlyphImage DrawCodeBox(char32_t Character, const Size &At);

}  // namespace typecase

#endif  // TYPECASE_CODE_BOX_H
