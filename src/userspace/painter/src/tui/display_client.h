#ifndef PAINTER_TUI_DISPLAY_CLIENT_H
#define PAINTER_TUI_DISPLAY_CLIENT_H

#include <cstdint>
#include <expected>

#include "box/cxx/bay.h"
#include "box/cxx/error.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"
#include "box/display.h"

namespace painter::tui {

/* One layer of the display, the whole screen in size. Open asks the display
 * how big the screen is and refuses one smaller than the least asked for, so
 * the checks every Painter surface needs are made here once. */
class DisplayLayer {
public:
    static std::expected<DisplayLayer, StageError> Open(std::int32_t LeastCols,
                                                        std::int32_t LeastRows,
                                                        DisplayLayerRole Role) noexcept;

    DisplayLayer(DisplayLayer &&) noexcept;
    DisplayLayer &operator=(DisplayLayer &&) noexcept;
    DisplayLayer(const DisplayLayer &) = delete;
    DisplayLayer &operator=(const DisplayLayer &) = delete;
    ~DisplayLayer();

    core::Size Size() const noexcept { return {Bounds.Width, Bounds.Height}; }
    bool IsOpen() const noexcept { return Opened; }
    GlassKind Glass() const noexcept { return Kind; }
    // Where the console's caret stood when the layer opened — after
    // everything this strand printed before opening it.
    core::Point Cursor() const noexcept { return ConsoleCursor; }

    // The picture is written straight into a slot of the layer's memory:
    // a transparent cell shows what lies beneath it, and the picture's caret
    // is the layer's.
    box::status Present(const Frame &Picture) noexcept;
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
    GlassKind Kind = GlassKind::Pixels;
    box::bay<std::byte> Shared;
    DisplayLayerSharedHeader *Header = nullptr;
    std::uint64_t NextSequence = 0;
    bool Opened = false;
};

}  // namespace painter::tui

#endif
