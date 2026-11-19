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
    Write(Picture, Area);
    Mark(Area);
}

void Mirror::Write(const painter::core::Canvas &Picture, Rect Area) noexcept
{
    Area = Clipped(Area);
    if (Area.Empty()) return;
    std::uint32_t *Into = All.data() + static_cast<std::size_t>(Area.Y) * Wide + static_cast<std::size_t>(Area.X);
    // A triple written as it is lies in the layout of BOX_PIXEL_BGR
    // (boxos_pixel.h): the mirror's.
    Picture.CopyOut(Rect{0, 0, Area.Width, Area.Height}, Into, Wide, BOX_PIXEL_BGR, painter::core::Dither::On,
                    painter::core::Point{Area.X, Area.Y});
}

void Mirror::Shift(Rect Area, std::int32_t Up, std::span<const Rect> Keep)
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
    /* What is kept, within the area, in order across: each row's stretches
     * between it found in one pass along the row. */
    std::vector<Rect> Across;
    Across.reserve(Keep.size());
    for (const Rect &Still : Keep)
        if (const Rect In = Still.Intersect(Area); !In.Empty()) Across.push_back(In);
    std::sort(Across.begin(), Across.end(), [](const Rect &A, const Rect &B) { return A.X < B.X; });
    const std::int32_t Right = Area.X + Area.Width;
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height - Up; Y++) {
        std::int32_t At = Area.X;
        for (const Rect &Still : Across) {
            if (Y < Still.Y || Y >= Still.Y + Still.Height) continue;
            if (Still.X > At) Move(Y, At, Still.X);
            At = std::max(At, Still.X + Still.Width);
        }
        if (At < Right) Move(Y, At, Right);
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
