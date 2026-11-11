/* paint.h — the sandbox as a picture: every cell a half-block pixel, the ring
 * of the brush over it, the palette and what is set at the bottom, and the
 * page of keys on top when asked for. */
#ifndef SAND_PAINT_H
#define SAND_PAINT_H

#include <vector>

#include "painter/tui/stage.h"
#include "sandbox.h"

namespace sand {

class Picture {
public:
    // Composed into the stage's frame; the caller presents it.
    void Draw(painter::tui::Stage &Screen, const Sandbox &Box);

private:
    std::vector<Color> Pixels;
};

}  // namespace sand

#endif
