/* canvas.cpp — the line the canvas draws. */

#include "canvas.h"

#include <cmath>
#include <cstdlib>

namespace forest {

void Canvas::Line(int X0, int Y0, int X1, int Y1, Color Ink)
{
    int Steps = std::max(std::abs(X1 - X0), std::abs(Y1 - Y0));
    for (int At = 0; At <= Steps; At++) {
        float T = Steps == 0 ? 0.0f : static_cast<float>(At) / static_cast<float>(Steps);
        Put(X0 + static_cast<int>(std::lround(static_cast<float>(X1 - X0) * T)),
            Y0 + static_cast<int>(std::lround(static_cast<float>(Y1 - Y0) * T)), Ink);
    }
}

}  // namespace forest
