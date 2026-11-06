#include "painter/tui/frame.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

#include "painter/tui/glyph.h"

namespace painter::tui {

box::result<Frame> Frame::Create(core::Size Requested) noexcept
{
    if (Requested.Empty() || Requested.Height > std::numeric_limits<std::int32_t>::max() / 2) {
        return std::unexpected(box::error{box::errc::out_of_range});
    }

    std::optional<std::size_t> Count = Requested.CellCount();
    if (!Count) return std::unexpected(box::error{box::errc::out_of_range});

    try {
        return Frame(Requested, std::vector<Cell>(*Count));
    } catch (const std::bad_alloc &) {
        return std::unexpected(box::error{box::errc::no_memory});
    } catch (const std::length_error &) {
        return std::unexpected(box::error{box::errc::out_of_range});
    }
}

std::int32_t Frame::PixelHeight() const noexcept
{
    return Bounds.Height * 2;
}

bool Frame::InBounds(core::Point At) const noexcept
{
    return At.X >= 0 && At.Y >= 0 && At.X < Bounds.Width && At.Y < Bounds.Height;
}

bool Frame::PixelInBounds(core::Point At) const noexcept
{
    return At.X >= 0 && At.Y >= 0 && At.X < Bounds.Width && At.Y < PixelHeight();
}

void Frame::Clear(TextStyle Style)
{
    std::fill(Cells.begin(), Cells.end(), Cell{' ', Style.Foreground, Style.Background});
}

void Frame::ClearTransparent() noexcept
{
    std::fill(Cells.begin(), Cells.end(), Cell::TransparentCell());
}

bool Frame::Put(core::Point At, Cell Value)
{
    if (!InBounds(At)) return false;
    this->At(At) = Value;
    return true;
}

bool Frame::Put(core::Point At, char Glyph, TextStyle Style)
{
    return Put(At, Cell{Glyph, Style.Foreground, Style.Background});
}

std::size_t Frame::Fill(core::Rect Area, Cell Value)
{
    core::Rect Clipped = Area.Intersect(Bounds);
    if (Clipped.Empty()) return 0;

    for (std::int32_t Row = Clipped.Y; Row < Clipped.Y + Clipped.Height; Row++) {
        for (std::int32_t Col = Clipped.X; Col < Clipped.X + Clipped.Width; Col++) {
            At({Col, Row}) = Value;
        }
    }
    return static_cast<std::size_t>(Clipped.Width) * static_cast<std::size_t>(Clipped.Height);
}

std::size_t Frame::Fill(core::Rect Area, char Glyph, TextStyle Style)
{
    return Fill(Area, Cell{Glyph, Style.Foreground, Style.Background});
}

std::size_t Frame::Write(core::Point At, std::string_view Text, TextStyle Style)
{
    if (At.Y < 0 || At.Y >= Bounds.Height) return 0;

    std::size_t Written = 0;
    for (char Glyph : Text) {
        if (At.X >= Bounds.Width) break;
        if (At.X >= 0) {
            this->At(At) = Cell{Glyph, Style.Foreground, Style.Background};
            Written++;
        }
        if (At.X == std::numeric_limits<std::int32_t>::max()) break;
        At.X++;
    }
    return Written;
}

std::size_t Frame::Centre(std::int32_t Row, std::string_view Text, TextStyle Style)
{
    std::int32_t Start = 0;
    if (Text.size() < static_cast<std::size_t>(Bounds.Width)) {
        Start = (Bounds.Width - static_cast<std::int32_t>(Text.size())) / 2;
    }
    return Write({Start, Row}, Text, Style);
}

bool Frame::PutPixel(core::Point Pixel, core::Color Ink)
{
    if (!PixelInBounds(Pixel)) return false;

    Cell &Target = At({Pixel.X, Pixel.Y / 2});
    Target.Glyph = Glyphs::UpperHalf;
    Target.VisibilityMode = Visibility::Opaque;
    if ((Pixel.Y & 1) == 0) Target.Foreground = Ink;
    else                    Target.Background = Ink;
    return true;
}

bool Frame::BlendPixel(core::Point Pixel, core::Color Ink, std::uint8_t Weight)
{
    if (!PixelInBounds(Pixel)) return false;

    Cell &Target = At({Pixel.X, Pixel.Y / 2});
    Target.Glyph = Glyphs::UpperHalf;
    Target.VisibilityMode = Visibility::Opaque;
    core::Color &Current = (Pixel.Y & 1) == 0 ? Target.Foreground : Target.Background;
    Current = core::Color::Mix(Current, Ink, Weight);
    return true;
}

bool Frame::PutPixels(std::span<const core::Color> Pixels)
{
    const std::optional<std::size_t> Count = Bounds.CellCount();
    if (!Count || Pixels.size() != *Count * 2u) return false;

    for (std::int32_t Row = 0; Row < Bounds.Height; Row++) {
        for (std::int32_t Col = 0; Col < Bounds.Width; Col++) {
            Cell &Target = At({Col, Row});
            Target.Glyph = Glyphs::UpperHalf;
            Target.VisibilityMode = Visibility::Opaque;
            Target.Foreground = Pixels[static_cast<std::size_t>(Row * 2) * Bounds.Width + Col];
            Target.Background = Pixels[static_cast<std::size_t>(Row * 2 + 1) * Bounds.Width + Col];
        }
    }
    return true;
}

Cell Frame::Read(core::Point At) const noexcept
{
    return InBounds(At) ? this->At(At) : Cell{};
}

}  // namespace painter::tui
