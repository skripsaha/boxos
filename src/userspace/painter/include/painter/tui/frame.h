#ifndef PAINTER_TUI_FRAME_H
#define PAINTER_TUI_FRAME_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "box/cxx/error.h"

#include "painter/core/color.h"
#include "painter/core/geometry.h"

namespace painter::tui {

struct TextStyle {
    core::Color Foreground = core::Colors::LightGray;
    core::Color Background = core::Colors::Black;
};

struct Cell {
    char        Glyph      = ' ';
    core::Color Foreground = core::Colors::LightGray;
    core::Color Background = core::Colors::Black;

    friend bool operator==(const Cell &, const Cell &) noexcept = default;
};

class Stage;
class Surface;

class Frame {
public:
    static box::result<Frame> Create(core::Size Bounds) noexcept;

    Frame(Frame &&) noexcept            = default;
    Frame &operator=(Frame &&) noexcept = default;
    Frame(const Frame &)                = delete;
    Frame &operator=(const Frame &)     = delete;
    ~Frame()                            = default;

    core::Size Size() const noexcept { return Bounds; }
    std::int32_t PixelHeight() const noexcept;

    void Clear(TextStyle Style = {});

    /* Cell operations clip at the frame boundary and report whether anything
     * was written. This keeps drawing convenient while making the boundary
     * behavior explicit. */
    bool        Put(core::Point At, Cell Value);
    bool        Put(core::Point At, char Glyph, TextStyle Style = {});
    std::size_t Fill(core::Rect Area, Cell Value);
    std::size_t Fill(core::Rect Area, char Glyph, TextStyle Style = {});
    std::size_t Write(core::Point At, std::string_view Text, TextStyle Style = {});
    std::size_t Centre(std::int32_t Row, std::string_view Text, TextStyle Style = {});

    /* Pixel coordinates are two rows per text cell. Drawing a pixel makes the
     * containing cell an upper-half glyph and changes only the selected half. */
    bool PutPixel(core::Point Pixel, core::Color Ink);
    bool BlendPixel(core::Point Pixel, core::Color Ink, std::uint8_t Weight);
    bool PutPixels(std::span<const core::Color> Pixels);

    Cell Read(core::Point At) const noexcept;

private:
    explicit Frame(core::Size Bounds, std::vector<Cell> Cells) noexcept
        : Bounds(Bounds), Cells(std::move(Cells))
    {
    }

    bool InBounds(core::Point At) const noexcept;
    bool PixelInBounds(core::Point At) const noexcept;
    Cell &At(core::Point At) noexcept { return Cells[static_cast<std::size_t>(At.Y) * Bounds.Width + At.X]; }
    const Cell &At(core::Point At) const noexcept
    {
        return Cells[static_cast<std::size_t>(At.Y) * Bounds.Width + At.X];
    }

    /* The kernel-facing representation stays private to Painter. */
    void WriteNative(void *Out, std::size_t Count) const noexcept;
    void WriteNativeRegion(void *Out, std::size_t Count, core::Rect Region) const noexcept;

    core::Size       Bounds;
    std::vector<Cell> Cells;

    friend class Stage;
    friend class Surface;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_FRAME_H
