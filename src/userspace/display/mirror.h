#ifndef BOXOS_DISPLAY_MIRROR_H
#define BOXOS_DISPLAY_MIRROR_H

// The mirror — a framebuffer's picture kept in this program's own memory, a
// triple a pixel: what the glass shows, or is to show once the easel puts it
// there (easel.h). A frame is put into it where it changed (Put); where the
// console scrolled it moves (Shift) round what stays still, each row compared
// with the row it takes before it is taken, so only the pixels that came out
// different are to go onto the glass. What waits to go is kept as each row's
// stretch, taken by the easel when it writes the glass — so video memory is
// written, never read, and no more of it than changed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "boxos_brush.h"
#include "painter/core/canvas.h"
#include "painter/core/geometry.h"

class Mirror {
public:
    // Width x Height pixels, black, none of them waiting. Throws
    // std::bad_alloc only.
    Mirror(std::uint32_t Width, std::uint32_t Height);

    std::uint32_t        Width() const noexcept { return Wide; }
    std::uint32_t        Height() const noexcept { return High; }
    const std::uint32_t *Pixels() const noexcept { return All.data(); }
    const std::uint32_t *Row(std::uint32_t Y) const noexcept { return All.data() + static_cast<std::size_t>(Y) * Wide; }

    // Area put in from Picture, whose corner lies at Area's corner, dithered
    // by the glass's own noise (painter/core/canvas.h CopyOut).
    void Put(const painter::core::Canvas &Picture, painter::core::Rect Area) noexcept;
    // Area moved up Up pixels, its last Up rows left as they were for a frame
    // to put — and the pixels within Keep left where they are: what lies
    // still over what moves.
    void Shift(painter::core::Rect Area, std::int32_t Up, std::span<const painter::core::Rect> Keep = {}) noexcept;
    // Count rectangles of a canvas as big as the mirror put in, pixel for
    // pixel (boxos_brush.h BoxBrushShow).
    void Show(const BoxBrushCanvas &Canvas, const BoxRect *Rects, std::uint32_t Count) noexcept;
    // Area, as far as the mirror goes, written as triples by Paint(Pixels,
    // Stride, Area) — Pixels its corner, Stride from row to row.
    template <typename Painting>
    void Paint(painter::core::Rect Area, Painting &&Each)
    {
        Area = Clipped(Area);
        if (Area.Empty()) return;
        Each(All.data() + static_cast<std::size_t>(Area.Y) * Wide + static_cast<std::size_t>(Area.X), static_cast<std::size_t>(Wide),
             Area);
        for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++) Wait(Y, Area.X, Area.X + Area.Width);
    }
    // All of it to go onto the glass; none of it.
    void Refresh() noexcept;
    void Forget() noexcept;

    // Each row's stretch waiting to go onto the glass told to Each(Y, From,
    // To), and none waiting from now on.
    template <typename Stretch>
    void TakeWaiting(Stretch &&Each)
    {
        for (std::uint32_t Y = 0; Y < High; Y++) {
            auto &Span = Waiting[Y];
            if (Span.first == Span.second) continue;
            const std::pair<std::int32_t, std::int32_t> Taken = Span;
            Span                                             = {0, 0};
            Each(Y, Taken.first, Taken.second);
        }
    }

private:
    void Wait(std::int32_t Y, std::int32_t From, std::int32_t To) noexcept;
    painter::core::Rect Clipped(painter::core::Rect Area) const noexcept;

    std::uint32_t                                     Wide = 0, High = 0;
    std::vector<std::uint32_t>                        All;
    std::vector<std::pair<std::int32_t, std::int32_t>> Waiting;   // each row's [From, To)
};

#endif
