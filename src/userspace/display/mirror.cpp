/*
 * mirror.cpp — a framebuffer's picture in this program's own memory. See
 * mirror.h.
 */

#include "mirror.h"

#include <algorithm>
#include <cstring>

#include "boxos_pixel.h"

using painter::core::Rect;

Mirror::Mirror(std::uint32_t Width, std::uint32_t Height)
    : Wide(Width), High(Height), All(static_cast<std::size_t>(Width) * Height, 0u)
{
}

Rect Mirror::Clipped(Rect Area) const noexcept
{
    return Area.Intersect(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
}

void Mirror::Put(const painter::core::Canvas &Picture, Rect Area) noexcept
{
    Area = Clipped(Area);
    if (Area.Empty()) return;
    std::uint32_t *Into = All.data() + static_cast<std::size_t>(Area.Y) * Wide + static_cast<std::size_t>(Area.X);
    // A triple written as it is lies in the layout of BOX_PIXEL_BGR
    // (boxos_pixel.h): the mirror's.
    Picture.CopyOut(Rect{0, 0, Area.Width, Area.Height}, Into, Wide, BOX_PIXEL_BGR, painter::core::Dither::On,
                    painter::core::Point{Area.X, Area.Y});
    Waiting.Add(Area);
}

void Mirror::Shift(Rect Area, std::int32_t Up, std::span<const Rect> Keep) noexcept
{
    Area = Clipped(Area);
    if (Area.Empty() || Up <= 0 || Up >= Area.Height) return;
    /* A stretch of row Y moved: compared with the row it takes before it is
     * taken, so only where the two differ does the glass need writing. */
    const auto Move = [&](std::int32_t Y, std::int32_t From, std::int32_t To) {
        std::uint32_t       *Into   = All.data() + static_cast<std::size_t>(Y) * Wide;
        const std::uint32_t *Taken  = Into + static_cast<std::size_t>(Up) * Wide;
        std::int32_t         First  = From;
        while (First < To && Into[First] == Taken[First]) First++;
        if (First == To) return;
        std::int32_t Last = To;
        while (Last > First && Into[Last - 1] == Taken[Last - 1]) Last--;
        std::memcpy(Into + First, Taken + First, static_cast<std::size_t>(Last - First) * sizeof(std::uint32_t));
        Waiting.Add(Rect{First, Y, Last - First, 1});
    };
    const std::int32_t Right = Area.X + Area.Width;
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height - Up; Y++) {
        /* The row's stretches between what is kept on it, left to right. */
        std::int32_t At = Area.X;
        while (At < Right) {
            std::int32_t Stop = Right, Resume = Right;
            for (const Rect &Still : Keep) {
                if (Y < Still.Y || Y >= Still.Y + Still.Height) continue;
                const std::int32_t L = std::max(Still.X, At), R = std::min(Still.X + Still.Width, Right);
                if (L >= R) continue;
                if (L < Stop) Stop = L, Resume = R;
                else if (L == Stop) Resume = std::max(Resume, R);
            }
            if (At < Stop) Move(Y, At, Stop);
            At = std::max(Resume, Stop == Right ? Right : At + 1);
        }
    }
}

void Mirror::Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept
{
    if (Canvas.Width != Wide || Canvas.Height != High) return;
    auto *Into = reinterpret_cast<volatile std::uint8_t *>(All.data());
    for (std::uint32_t I = 0; I < Count; I++) {
        BoxBrushShow(&Canvas, Rects[I], Into, Wide * 4u, BOX_PIXEL_BGR);
        Waiting.Add(Clipped(Rect{Rects[I].X, Rects[I].Y, Rects[I].Width, Rects[I].Height}));
    }
}

void Mirror::Refresh() noexcept
{
    Waiting.Clear();
    Waiting.Add(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
}

void Mirror::Forget() noexcept
{
    Waiting.Clear();
}
