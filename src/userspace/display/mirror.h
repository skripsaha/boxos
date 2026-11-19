#ifndef BOXOS_DISPLAY_MIRROR_H
#define BOXOS_DISPLAY_MIRROR_H

// The mirror — a framebuffer's picture kept in this program's own memory, a
// triple a pixel: what the glass shows, or is to show once the easel puts it
// there (easel.h). A frame is put into it where it changed (Put); where the
// console scrolled it moves (Shift) round what stays still, each row compared
// with the row it takes before it is taken, so only the pixels that came out
// different are to go onto the glass. What waits to go is kept exactly, as a
// region (region.h), taken by the easel when it writes the glass — so video
// memory is written, never read, and no more of it than changed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "boxos_brush.h"
#include "painter/core/canvas.h"
#include "painter/core/geometry.h"
#include "region.h"

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
    // by the glass's own noise (painter/core/canvas.h CopyOut), and marked to
    // go onto the glass: Write, then Mark.
    void Put(const painter::core::Canvas &Picture, painter::core::Rect Area) noexcept;
    // Area's pixels written from Picture as Put writes them, nothing marked:
    // on any strand, each writing areas of its own.
    void Write(const painter::core::Canvas &Picture, painter::core::Rect Area) noexcept;
    // Area to go onto the glass.
    void Mark(painter::core::Rect Area) noexcept { Waiting.Add(Clipped(Area)); }
    // Area moved up Up pixels, its last Up rows left as they were for a frame
    // to put — and the pixels within Keep left where they are: what lies
    // still over what moves, however much of it. Throws std::bad_alloc only,
    // part of it moved: then all of it is to be drawn again.
    void Shift(painter::core::Rect Area, std::int32_t Up, std::span<const painter::core::Rect> Keep = {});
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
        Waiting.Add(Area);
    }
    // All of it to go onto the glass; none of it.
    void Refresh() noexcept;
    void Forget() noexcept;

    // Each stretch of a row waiting to go onto the glass told to Tell(Y,
    // From, To), and none waiting from now on — all of it, when there is no
    // room to know what waits.
    template <typename Stretch>
    void TakeWaiting(Stretch &&Tell) noexcept
    {
        std::vector<painter::core::Rect> Taken;
        try {
            Taken = Waiting.Take();
        } catch (...) {
            Waiting.Clear();
            for (std::uint32_t Y = 0; Y < High; Y++) Tell(Y, 0, static_cast<std::int32_t>(Wide));
            return;
        }
        for (const painter::core::Rect &Each : Taken)
            for (std::int32_t Y = Each.Y; Y < Each.Y + Each.Height; Y++) Tell(static_cast<std::uint32_t>(Y), Each.X, Each.X + Each.Width);
    }

private:
    painter::core::Rect Clipped(painter::core::Rect Area) const noexcept;

    std::uint32_t              Wide = 0, High = 0;
    std::vector<std::uint32_t> All;
    items::Region              Waiting;   // what is to go onto the glass
};

#endif
