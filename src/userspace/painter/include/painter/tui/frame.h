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

/* What the glass is that a frame will be shown on, as the display says it
 * (box/display.h DisplayGlassKind): a text page of 16 colours, or a
 * framebuffer. Both draw the same cells; only what a cell can look like
 * differs, so a program that softens edges asks it. */
enum class GlassKind : std::uint8_t {
    Text   = DISPLAY_GLASS_TEXT,
    Pixels = DISPLAY_GLASS_PIXELS,
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
    char32_t    Glyph      = U' ';   /* the character it shows, not a glyph's number */
    core::Color Foreground = core::Colors::LightGray;
    core::Color Background = core::Colors::Black;
    Visibility  VisibilityMode = Visibility::Opaque;
    CellMark    Mark       = CellMark::None;
    core::Color MarkColor  = core::Colors::LightGray;

    /* A cell that shows nothing: its colours are see-through, so a pixel put
     * into it leaves the other half of the cell to what lies beneath. */
    static constexpr Cell TransparentCell() noexcept
    {
        return {U' ', core::Color::Through(), core::Color::Through(), Visibility::Transparent};
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

    /* The glass this frame is for. A Stage gives its frame the glass it holds;
     * a frame made on its own is for pixels until it is told otherwise. */
    GlassKind Glass() const noexcept { return Kind; }
    void      SetGlass(GlassKind Value) noexcept { Kind = Value; }

    /* The grids finer than the halves the glass draws exactly, as the display
     * said them (box/display.h DisplayMosaic): none until told. */
    std::uint8_t Mosaics() const noexcept { return MosaicFlags; }
    void         SetMosaics(std::uint8_t Value) noexcept { MosaicFlags = Value; }

    /* Clear writes an opaque background. ClearTransparent leaves every cell
     * transparent when this frame is passed to an overlay replacement. */
    void Clear(TextStyle Style = {});
    void ClearTransparent() noexcept;

    /* Cell operations clip at the frame boundary and report whether anything
     * was written. This keeps drawing convenient while making the boundary
     * behavior explicit. Write and Centre take UTF-8 text and lay it as
     * text.h measures it: a character to a cell, a tab to the next stop, and
     * a control or a byte that begins no character as U+FFFD. */
    bool        Put(core::Point At, Cell Value);
    bool        Put(core::Point At, char32_t Glyph, TextStyle Style = {});
    std::size_t Fill(core::Rect Area, Cell Value);
    std::size_t Fill(core::Rect Area, char32_t Glyph, TextStyle Style = {});
    std::size_t Write(core::Point At, std::string_view Text, TextStyle Style = {});
    std::size_t Centre(std::int32_t Row, std::string_view Text, TextStyle Style = {});

    /* Pixel coordinates are two rows per text cell. Drawing a pixel makes the
     * containing cell an upper-half glyph and changes only the selected half;
     * in a cell never painted before on a transparent frame, the other half
     * stays see-through. A blend over a see-through half is the ink at that
     * weight as its opacity, over a translucent one the two composed
     * (Color::Over); an Ink with an opacity is put as it is. */
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
    GlassKind                  Kind = GlassKind::Pixels;
    std::uint8_t               MosaicFlags = 0;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_FRAME_H
