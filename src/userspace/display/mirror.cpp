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
    : Wide(Width), High(Height), All(static_cast<std::size_t>(Width) * Height, 0u), Waiting(Height, {0, 0})
{
}

Rect Mirror::Clipped(Rect Area) const noexcept
{
    return Area.Intersect(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)});
}

void Mirror::Wait(std::int32_t Y, std::int32_t From, std::int32_t To) noexcept
{
    if (Y < 0 || static_cast<std::uint32_t>(Y) >= High || From >= To) return;
    auto &Span = Waiting[static_cast<std::size_t>(Y)];
    Span       = Span.first == Span.second ? std::pair{From, To} : std::pair{std::min(Span.first, From), std::max(Span.second, To)};
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
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++) Wait(Y, Area.X, Area.X + Area.Width);
}

void Mirror::Shift(Rect Area, std::int32_t Up) noexcept
{
    Area = Clipped(Area);
    if (Area.Empty() || Up <= 0 || Up >= Area.Height) return;
    const std::size_t Count = static_cast<std::size_t>(Area.Width);
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height - Up; Y++) {
        std::uint32_t       *To    = All.data() + static_cast<std::size_t>(Y) * Wide + static_cast<std::size_t>(Area.X);
        const std::uint32_t *From  = To + static_cast<std::size_t>(Up) * Wide;
        std::size_t          First = 0;
        while (First < Count && To[First] == From[First]) First++;
        if (First == Count) continue;
        std::size_t Last = Count;
        while (Last > First && To[Last - 1] == From[Last - 1]) Last--;
        std::memcpy(To + First, From + First, (Last - First) * sizeof(std::uint32_t));
        Wait(Y, Area.X + static_cast<std::int32_t>(First), Area.X + static_cast<std::int32_t>(Last));
    }
}

void Mirror::Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept
{
    if (Canvas.Width != Wide || Canvas.Height != High) return;
    auto *Into = reinterpret_cast<volatile std::uint8_t *>(All.data());
    for (std::uint32_t I = 0; I < Count; I++) {
        BoxBrushShow(&Canvas, Rects[I], Into, Wide * 4u, BOX_PIXEL_BGR);
        const Rect Area = Clipped(Rect{Rects[I].X, Rects[I].Y, Rects[I].Width, Rects[I].Height});
        for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++) Wait(Y, Area.X, Area.X + Area.Width);
    }
}

void Mirror::Refresh() noexcept
{
    for (auto &Span : Waiting) Span = {0, static_cast<std::int32_t>(Wide)};
}

void Mirror::Forget() noexcept
{
    for (auto &Span : Waiting) Span = {0, 0};
}
