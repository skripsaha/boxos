#ifndef TYPECASE_GLYPH_LOOK_H
#define TYPECASE_GLYPH_LOOK_H

/* How a font's glyph is drawn for text — at a size, its pen at a quarter of
 * a pixel's place across, made bold or leaning where its family has no face
 * that is — and the drawing itself: the picture, or the outline the picture
 * is covered from.
 *
 * Glyphs are not hinted: an outline lands on the pixels where its design
 * puts it, and a flat stroke at the height of the lowercase falls across two
 * rows as two grey ones. So the type is set a hair taller or shorter — the
 * lowercase a whole number of pixels high over a baseline that is a row's
 * edge — and the tops of a, n, x, z are one sharp edge. Across, the scale is
 * kept, and with it every advance.
 *
 * Bold made of a face that is not: its outline grown outward on every side —
 * by a 24th of the em in all at 9 pixels to the em and less, a 32nd at 36
 * and more — its left edge and baseline kept, its advance longer by as much.
 * Leaning made of an upright face: its outline slanted 12° about the
 * baseline, its advance kept. */

#include <cstdint>

#include "typecase/font.h"
#include "typecase/glyph_image.h"
#include "typecase/outline.h"
#include "typecase/rasterizer.h"

namespace typecase {

struct GlyphLook {
    float        Em    = 0;       /* pixels to the em */
    std::uint8_t Phase = 0;       /* the pen this many quarters of a pixel right of a pixel's edge, 0 to 3 */
    bool         Bold  = false;   /* made bold */
    bool         Slant = false;   /* made to lean */

    bool operator==(const GlyphLook &) const = default;
};

/* How much longer bold makes an advance at Em, in pixels. */
float BoldGrowth(float Em) noexcept;

/* The glyph's advance as Look draws it, in pixels. */
float AdvanceOf(const Font &Face, GlyphId Glyph, const GlyphLook &Look) noexcept;

/* The glyph's outline as Look draws it, in pixels against the pen, x right
 * and y down as a picture counts them — the very outline its picture is
 * covered from. Empty for a glyph of no outline, or of one that does not
 * read. */
Outline GlyphOutline(const Font &Face, GlyphId Glyph, const GlyphLook &Look);

/* The glyph's picture as Look draws it, no part outside Limit — or, with no
 * Limit, outside two ems round the pen and its advance: a damaged font asks
 * for no more pixels than its size gives. */
GlyphImage DrawGlyph(const Font &Face, GlyphId Glyph, const GlyphLook &Look, Bounds Limit, Rasterizer &Pen);
GlyphImage DrawGlyph(const Font &Face, GlyphId Glyph, const GlyphLook &Look, Rasterizer &Pen);

}  // namespace typecase

#endif  // TYPECASE_GLYPH_LOOK_H
