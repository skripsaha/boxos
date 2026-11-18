/*
 * pixels.cpp — Duppo in the screen's own pixels. See pixels.h.
 */

#include "pixels.h"

#include <algorithm>
#include <cmath>

#include "painter/core/pixel.h"

#include "sampler.h"

namespace duppo {

namespace {

using painter::core::Rect;

/* Beyond the logo's reach: its squash, its bubbles swung out by its turn and
 * left behind by its way, in shares of the reach and in glass units. */
constexpr float Swing = 1.15f;
constexpr float Lag   = 3.0f;

}  // namespace

painter::core::Size Pixels::PageFor(float Size, float Unit)
{
    const float Reach = std::max({Logo::ReachLeft, Logo::ReachRight, Logo::ReachUp, Logo::ReachDown}) * Swing * Size + Lag;
    const auto  Side  = static_cast<std::int32_t>(std::ceil(2.0f * Reach * Unit)) + 2;
    return painter::core::Size{Side, Side};
}

Rect Pixels::Touched(const Flight &Body, const Orbit &Moons, painter::core::Size Page, Vec Corner, float Unit)
{
    const Reach        Of   = ReachOf(Body, Moons);
    const std::int32_t Left = static_cast<std::int32_t>(std::floor((Of.Left - Corner.X) * Unit)) - 1;
    const std::int32_t Top  = static_cast<std::int32_t>(std::floor((Of.Top - Corner.Y) * Unit)) - 1;
    const std::int32_t Right  = static_cast<std::int32_t>(std::ceil((Of.Right - Corner.X) * Unit)) + 1;
    const std::int32_t Bottom = static_cast<std::int32_t>(std::ceil((Of.Bottom - Corner.Y) * Unit)) + 1;
    return Rect{Left, Top, Right - Left, Bottom - Top}.Intersect(Rect{0, 0, Page.Width, Page.Height});
}

void Pixels::Draw(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, painter::core::Canvas &Page,
                  Vec Corner, float Unit, Rect Area) const
{
    Page.Clear(Area, Color::Through());
    const Sampler Seen(Body, Eye, Turning, Moons, Colours, !Softened, Vec{1.0f / Unit, 1.0f / Unit});
    const Rect    Within = Touched(Body, Moons, {static_cast<std::int32_t>(Page.Width()), static_cast<std::int32_t>(Page.Height())},
                                   Corner, Unit)
                            .Intersect(Area);
    for (std::int32_t Y = Within.Y; Y < Within.Y + Within.Height; Y++) {
        painter::core::Pixel *Row  = Page.Row(static_cast<std::uint32_t>(Y));
        const float           Down = Corner.Y + (static_cast<float>(Y) + 0.5f) / Unit;
        for (std::int32_t X = Within.X; X < Within.X + Within.Width; X++) {
            bool        Here = false;
            const Color Ink  = Seen.At(Vec{Corner.X + (static_cast<float>(X) + 0.5f) / Unit, Down}, Here);
            if (Here) Row[X] = painter::core::Pixel::Of(Ink);
        }
    }
}

}  // namespace duppo
