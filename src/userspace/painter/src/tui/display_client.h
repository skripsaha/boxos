#ifndef PAINTER_TUI_DISPLAY_CLIENT_H
#define PAINTER_TUI_DISPLAY_CLIENT_H

#include <cstdint>
#include <expected>
#include <span>

#include "box/cxx/bay.h"
#include "box/cxx/error.h"
#include "painter/core/geometry.h"
#include "box/display.h"

namespace painter::tui {

class DisplayLayer {
public:
    static std::expected<DisplayLayer, box::error> Open(core::Rect Bounds,
                                                         DisplayLayerRole Role) noexcept;

    DisplayLayer(DisplayLayer &&) noexcept;
    DisplayLayer &operator=(DisplayLayer &&) noexcept;
    DisplayLayer(const DisplayLayer &) = delete;
    DisplayLayer &operator=(const DisplayLayer &) = delete;
    ~DisplayLayer();

    core::Size Size() const noexcept { return {Bounds.Width, Bounds.Height}; }
    bool IsOpen() const noexcept { return Opened; }
    // Where the console's caret stood when the layer opened — after
    // everything this strand printed before opening it.
    core::Point Cursor() const noexcept { return ConsoleCursor; }

    box::status Present(std::span<const DisplayLayerCell> Cells,
                        core::Point Cursor, bool CursorVisible) noexcept;
    box::status Commit(std::uint32_t ScrollRows = 0) noexcept;
    box::status Close() noexcept;

private:
    DisplayLayer(core::Rect BoundsValue, const DisplayLayerOpenReply &Reply,
                 box::bay<std::byte> SharedValue) noexcept;

    box::status Notify() noexcept;

    core::Rect Bounds;
    std::uint32_t DisplayPid = 0;
    std::uint32_t LayerId = 0;
    core::Point ConsoleCursor;
    box::bay<std::byte> Shared;
    DisplayLayerSharedHeader *Header = nullptr;
    std::uint64_t NextSequence = 0;
    bool Opened = false;
};

}  // namespace painter::tui

#endif
