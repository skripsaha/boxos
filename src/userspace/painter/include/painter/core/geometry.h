#ifndef PAINTER_CORE_GEOMETRY_H
#define PAINTER_CORE_GEOMETRY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace painter::core {

struct Point {
    std::int32_t X = 0;
    std::int32_t Y = 0;

    friend constexpr bool operator==(Point, Point) noexcept = default;
};

struct Size {
    std::int32_t Width  = 0;
    std::int32_t Height = 0;

    constexpr bool Empty() const noexcept { return Width <= 0 || Height <= 0; }

    std::optional<std::size_t> CellCount() const noexcept
    {
        if (Empty()) return std::nullopt;
        const std::uint64_t Wide = static_cast<std::uint64_t>(Width);
        const std::uint64_t High = static_cast<std::uint64_t>(Height);
        const std::uint64_t Count = Wide * High;
        if (Count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
        return static_cast<std::size_t>(Count);
    }

    friend constexpr bool operator==(Size, Size) noexcept = default;
};

struct Rect {
    std::int32_t X      = 0;
    std::int32_t Y      = 0;
    std::int32_t Width  = 0;
    std::int32_t Height = 0;

    constexpr bool Empty() const noexcept { return Width <= 0 || Height <= 0; }

    constexpr Rect Intersect(Rect Other) const noexcept
    {
        const std::int64_t Left   = std::max<std::int64_t>(X, Other.X);
        const std::int64_t Top    = std::max<std::int64_t>(Y, Other.Y);
        const std::int64_t Right  = std::min<std::int64_t>(static_cast<std::int64_t>(X) + Width,
                                                            static_cast<std::int64_t>(Other.X) + Other.Width);
        const std::int64_t Bottom = std::min<std::int64_t>(static_cast<std::int64_t>(Y) + Height,
                                                            static_cast<std::int64_t>(Other.Y) + Other.Height);
        if (Right <= Left || Bottom <= Top) return {};
        const std::int64_t Wide = std::min<std::int64_t>(Right - Left, std::numeric_limits<std::int32_t>::max());
        const std::int64_t High = std::min<std::int64_t>(Bottom - Top, std::numeric_limits<std::int32_t>::max());
        return {static_cast<std::int32_t>(Left), static_cast<std::int32_t>(Top),
                static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)};
    }

    constexpr Rect Intersect(Size Bounds) const noexcept
    {
        return Intersect(Rect{0, 0, Bounds.Width, Bounds.Height});
    }

    friend constexpr bool operator==(Rect, Rect) noexcept = default;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_GEOMETRY_H
