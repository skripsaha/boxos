/* field.cpp — the wax added up over a grid. See field.h.
 *
 * A blob's share at a point is (1 - q)^3 for q, the point's distance from its
 * middle over its reach, squared, below 1 — none at its reach and beyond, and
 * as smooth there as in its middle, so the wax's glow fades out with no edge.
 * Read against 0.2 its edge lies at sqrt(1 - 0.2^(1/3)) = 0.644 of its reach:
 * a blob of radius R reaches R / 0.644, and the level is 5 times that share,
 * 1 at its edge. */

#include "field.h"

#include <algorithm>
#include <cmath>

namespace lavalamp {

namespace {

constexpr float EdgeOfReach = 0.6444f;
constexpr float PerShare    = 5.0f;

float Share(float Q)
{
    const float Left = 1.0f - Q;
    return Left * Left * Left * PerShare;
}

}  // namespace

void Field::Lay(int Across, int Down, float FromXAt, float FromYAt, float StepXBy, float StepYBy)
{
    Wide  = std::max(Across, 1);
    High  = std::max(Down, 1);
    FromX = FromXAt;
    FromY = FromYAt;
    StepX = StepXBy;
    StepY = StepYBy;
    Levels.assign(static_cast<std::size_t>(Wide) * High, 0.0f);
    Warmth.assign(Levels.size(), 0.0f);
}

float Field::Heat(int X, int Y) const noexcept
{
    const std::size_t At    = static_cast<std::size_t>(Y) * Wide + X;
    const float       Level = Levels[At];
    return Level > 1e-4f ? std::min(Warmth[At] / Level, 1.0f) : 0.5f;
}

void Field::Fill(const Lamp &Of)
{
    std::fill(Levels.begin(), Levels.end(), 0.0f);
    std::fill(Warmth.begin(), Warmth.end(), 0.0f);
    for (const Blob &One : Of.Blobs()) AddBlob(One);
    AddPool(Of);
}

/* A blob's wax over the points within its reach, an ellipse its radii make. */
void Field::AddBlob(const Blob &One)
{
    const float ReachX = One.Across / EdgeOfReach, ReachY = One.Down / EdgeOfReach;
    const int   Left   = std::max(0, static_cast<int>(std::ceil((One.X - ReachX - FromX) / StepX)));
    const int   Right  = std::min(Wide - 1, static_cast<int>(std::floor((One.X + ReachX - FromX) / StepX)));
    const int   Top    = std::max(0, static_cast<int>(std::ceil((One.Y - ReachY - FromY) / StepY)));
    const int   Bottom = std::min(High - 1, static_cast<int>(std::floor((One.Y + ReachY - FromY) / StepY)));
    const float InX = StepX / ReachX, StartX = (X(Left) - One.X) / ReachX;
    for (int Row = Top; Row <= Bottom; Row++) {
        const float Dy  = (Y(Row) - One.Y) / ReachY;
        const float Dy2 = Dy * Dy;
        if (Dy2 >= 1.0f) continue;
        float      *Level = &Levels[static_cast<std::size_t>(Row) * Wide];
        float      *Warm  = &Warmth[static_cast<std::size_t>(Row) * Wide];
        for (int Col = Left; Col <= Right; Col++) {
            const float Dx = StartX + static_cast<float>(Col - Left) * InX;
            const float Q  = Dx * Dx + Dy2;
            if (Q >= 1.0f) continue;
            const float Got = Share(Q);
            Level[Col] += Got;
            Warm[Col] += Got * One.Heat;
        }
    }
}

/* The pool: a soft floor of warm wax whose top waves along the bottom. Below
 * its middle line, just under the glass, it is wax through and through. */
void Field::AddPool(const Lamp &Of)
{
    constexpr float Middle = 1.02f;
    for (int Col = 0; Col < Wide; Col++) {
        const float Reach = (Middle - Of.PoolTop(X(Col))) / EdgeOfReach;
        const int   Top   = std::max(0, static_cast<int>(std::ceil((Middle - Reach - FromY) / StepY)));
        for (int Row = Top; Row < High; Row++) {
            const float D   = (Middle - Y(Row)) / Reach;
            if (D >= 1.0f) continue;
            const float Got = Share(D > 0 ? D * D : 0.0f);
            const std::size_t At = static_cast<std::size_t>(Row) * Wide + Col;
            Levels[At] += Got;
            Warmth[At] += Got;
        }
    }
}

}  // namespace lavalamp
