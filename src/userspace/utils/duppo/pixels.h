/* pixels.h — Duppo in the screen's own pixels, on a framebuffer: every pixel
 * it reaches sampled once by the sampler (sampler.h), its edges softened over
 * the pixel itself, on a canvas of Duppo's own that the display lays on the
 * glass where Duppo is. The pixels it misses are see-through, so the screen
 * goes on beneath it. */
#ifndef DUPPO_PIXELS_H
#define DUPPO_PIXELS_H

#include "painter/core/canvas.h"
#include "painter/core/geometry.h"

#include "face.h"
#include "flight.h"
#include "logo.h"
#include "orbit.h"
#include "spin.h"

namespace duppo {

class Pixels {
public:
    /* Its colours, and whether its edge is soft; if not, every edge pixel is
     * whole or none. */
    void Dress(const Palette &Hues, bool Smooth)
    {
        Colours  = Hues;
        Softened = Smooth;
    }

    /* The canvas Duppo of Size needs at Unit pixels a glass unit: square, its
     * middle Duppo's, wide enough for its bubbles however it turns, its
     * squash, the way its bubbles lag behind it, and a soft edge. */
    static painter::core::Size PageFor(float Size, float Unit);

    /* The pixels of Page — its corner at Corner of the glass, in glass units —
     * Duppo touches now. */
    static painter::core::Rect Touched(const Flight &Body, const Orbit &Moons, painter::core::Size Page, Vec Corner, float Unit);

    /* Duppo drawn onto Page within Area: Area cleared, and every pixel of it
     * Duppo touches drawn. */
    void Draw(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, painter::core::Canvas &Page,
              Vec Corner, float Unit, painter::core::Rect Area) const;

private:
    Palette Colours;
    bool    Softened = true;
};

}  // namespace duppo

#endif
