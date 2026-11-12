/* picture.h — Duppo on the glass: each cell it reaches sampled at its four
 * sub-pixels (two across, two down), each sampled through the logo's own
 * shading, and settled by Painter on the glyph and two colours that show them
 * best. The eye is a picture laid on the ball and turned with it, so it
 * narrows toward the edge and goes round it; the little bubbles lie before or
 * behind the ball by their depth, sample by sample. An edge, within the figure
 * and its own, is softened by the share of a sub-pixel each disc takes; at the
 * figure's edge that share is an opacity, for what lies beyond it is not known,
 * and the display blends it with whatever lies beneath. The cells it misses
 * are transparent, so the screen goes on beneath it. */
#ifndef DUPPO_PICTURE_H
#define DUPPO_PICTURE_H

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "face.h"
#include "flight.h"
#include "logo.h"
#include "orbit.h"
#include "spin.h"

namespace duppo {

class Picture {
public:
    explicit Picture(painter::core::Size Cells) : Cells(Cells) {}

    /* Its colours, and whether its edge is soft; if not, every edge sub-pixel
     * is whole or none, as on a text page. */
    void Dress(const Palette &Hues, bool Smooth)
    {
        Colours  = Hues;
        Softened = Smooth;
    }

    /* Duppo drawn into Overlay, which is as large as the glass; the cells that
     * changed since the last time, to composite. */
    painter::core::Rect Draw(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons,
                             painter::tui::Frame &Overlay);

private:
    painter::core::Size Cells;
    Palette             Colours;
    bool                Softened = true;
    painter::core::Rect Shown;
};

}  // namespace duppo

#endif
