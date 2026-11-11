/* view.h — where the scene lies on the glass. The scene is drawn in its own
 * units, 100 high with the ground at the bottom, and a View turns them into the
 * units of the frame (a cell is 1 across and 2 down, as PutPixel and FineCanvas
 * count), so a circle in the scene is round on the glass at any size. */
#ifndef HANGMAN_VIEW_H
#define HANGMAN_VIEW_H

#include <algorithm>
#include <cstdint>

#include "painter/core/geometry.h"

namespace hangman {

inline constexpr float SceneHigh = 100.0f;
inline constexpr float SceneLeast = 80.0f;   // the least width of the scene, in its units

struct View {
    float Left;     // the scene's corner in frame units
    float Top;
    float Wide;     // its size in frame units
    float High;
    float Scale;    // frame units to a unit of the scene
    float Across;   // the scene's width in its units
    float Ground;   // the frame row where the scene's 0 is

    static View Fit(painter::core::Rect Cells)
    {
        View Shown{};
        Shown.Left   = static_cast<float>(Cells.X);
        Shown.Top    = static_cast<float>(Cells.Y * 2);
        Shown.Wide   = static_cast<float>(Cells.Width);
        Shown.High   = static_cast<float>(Cells.Height * 2);
        Shown.Scale  = std::min(Shown.High / SceneHigh, Shown.Wide / SceneLeast);
        Shown.Across = Shown.Wide / Shown.Scale;
        Shown.Ground = Shown.Top + Shown.High - SceneHigh * Shown.Scale;
        return Shown;
    }

    painter::core::PointF At(float X, float Y) const { return {Left + X * Scale, Ground + Y * Scale}; }
    float                 Span(float Length) const { return Length * Scale; }
};

}  // namespace hangman

#endif
