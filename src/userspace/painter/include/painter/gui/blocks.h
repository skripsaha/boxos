#ifndef PAINTER_GUI_BLOCKS_H
#define PAINTER_GUI_BLOCKS_H

// Blocks of memory the display makes for a program alone (painter/gui/area.h
// OpenPicture, OpenCanvas): a picture's pixels, and a canvas's two pages of
// the program's own pixels.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "box/cxx/bay.h"
#include "painter/core/canvas.h"

namespace painter::gui {

// A picture: Width x Height pixels of R, G, B, A bytes, the opacity not
// multiplied in. Filled, then shown by the items that name it; changed, and
// said so (Changes::BlockChanged).
struct PictureBlock {
    std::uint32_t          Id = 0;
    std::uint32_t          Width = 0, Height = 0;
    box::bay<std::byte>    Memory;
    std::span<std::uint8_t> Pixels() noexcept;
};

// A canvas of the program's own pixels: two pages, one drawn while the other
// is shown — a page is drawn in only while it is free (Free), and given to the
// display with Changes::BlockChanged naming it.
struct CanvasBlock {
    std::uint32_t       Id = 0;
    std::uint32_t       Width = 0, Height = 0;
    box::bay<std::byte> Memory;
    bool                         Free(std::uint32_t Page) const noexcept;
    std::optional<core::Canvas> Page(std::uint32_t Page) noexcept;   // to draw on, while free
    void                         Taken(std::uint32_t Page) noexcept;  // given to the display
    // A page's pixels whatever it is doing, and how many bytes a page holds.
    core::Pixel *PageAt(std::uint32_t Page) noexcept;
    std::size_t  PageBytes() const noexcept;
};

}  // namespace painter::gui

#endif  // PAINTER_GUI_BLOCKS_H
