#ifndef PAINTER_TUI_LAYER_CANVAS_H
#define PAINTER_TUI_LAYER_CANVAS_H

#include <cstdint>
#include <expected>
#include <memory>

#include "box/display.h"
#include "painter/tui/stage.h"

namespace painter::tui {

/* Internal retained canvas shared by the two explicit output contracts:
 * Surface is temporary and ConsoleSurface is committed by the TUI target. */
class LayerCanvas {
public:
    static std::expected<LayerCanvas, StageError> Open(std::int32_t LeastCols,
                                                        std::int32_t LeastRows,
                                                        DisplayLayerRole Role) noexcept;

    LayerCanvas(LayerCanvas &&) noexcept;
    LayerCanvas &operator=(LayerCanvas &&) noexcept;
    LayerCanvas(const LayerCanvas &) = delete;
    LayerCanvas &operator=(const LayerCanvas &) = delete;
    ~LayerCanvas();

    core::Size Size() const noexcept;
    bool IsOpen() const noexcept;
    std::expected<core::Point, box::error> Cursor() const noexcept;

    box::status Paint(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Paint(core::Point Origin, const Frame &FrameBuffer,
                      core::Rect Region) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer,
                          core::Rect Region) noexcept;
    box::status Clear() noexcept;
    box::status SetCursor(core::Point At) noexcept;
    box::status Commit(std::uint32_t ScrollRows = 0) noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit LayerCanvas(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_LAYER_CANVAS_H
