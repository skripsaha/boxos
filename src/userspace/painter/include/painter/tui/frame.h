#ifndef PAINTER_TUI_FRAME_H
#define PAINTER_TUI_FRAME_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "box/cxx/error.h"
#include "box/display.h"

#include "painter/core/color.h"
#include "painter/core/geometry.h"

namespace painter::tui {

struct TextStyle {
    core::Color Foreground = core::Colors::LightGray;
    core::Color Background = core::Colors::Black;
};

enum class Visibility : std::uint8_t {
    Transparent,
    Opaque,
};

/* A mark the display draws over a cell's glyph in a colour of its own, so a
 * caret can stand on a letter and cover nothing: a line under the glyph, or a
 * bar down its left side (box/display.h DisplayCellMark). It is part of the
 * picture only — folded into the console's text, a cell keeps no mark. */
enum class CellMark : std::uint8_t {
    None  = DISPLAY_CELL_MARK_NONE,
    Under = DISPLAY_CELL_MARK_UNDER,
    Bar   = DISPLAY_CELL_MARK_BAR,
};

struct Cell {
    char        Glyph      = ' ';
    core::Color Foreground = core::Colors::LightGray;
    core::Color Background = core::Colors::Black;
    Visibility  VisibilityMode = Visibility::Opaque;
    CellMark    Mark       = CellMark::None;
    core::Color MarkColor  = core::Colors::LightGray;

    static constexpr Cell TransparentCell() noexcept
    {
        return {' ', core::Colors::LightGray, core::Colors::Black, Visibility::Transparent};
    }

    bool IsTransparent() const noexcept { return VisibilityMode == Visibility::Transparent; }

    friend bool operator==(const Cell &, const Cell &) noexcept = default;
};

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

    /* Clear writes an opaque background. ClearTransparent leaves every cell
     * transparent when this frame is passed to an overlay replacement. */
    void Clear(TextStyle Style = {});
    void ClearTransparent() noexcept;

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

    /* The caret is part of the picture: the one mark a cell cannot draw, a
     * line under the glyph where text goes. A frame has none until one is
     * placed, and Clear and ClearTransparent take it away with the rest.
     * Placed outside the frame it is placed nowhere, and false says so. */
    bool                       SetCaret(std::optional<core::Point> At) noexcept;
    std::optional<core::Point> Caret() const noexcept { return CaretAt; }

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

    core::Size                 Bounds;
    std::vector<Cell>          Cells;
    std::optional<core::Point> CaretAt;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_FRAME_H
