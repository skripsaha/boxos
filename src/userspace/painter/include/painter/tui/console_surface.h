#ifndef PAINTER_TUI_CONSOLE_SURFACE_H
#define PAINTER_TUI_CONSOLE_SURFACE_H

#include <cstdint>
#include <expected>
#include <memory>

#include "painter/tui/stage.h"

namespace painter::tui {

/* A TUI target transaction. It paints through an area of its own over the
 * console while it is being assembled, then Commit() folds the result into
 * the console's text and closes the area atomically. */
class ConsoleSurface {
public:
    static std::expected<ConsoleSurface, StageError> Open(std::int32_t LeastCols = 0,
                                                          std::int32_t LeastRows = 0) noexcept;

    ConsoleSurface(ConsoleSurface &&) noexcept;
    ConsoleSurface &operator=(ConsoleSurface &&) noexcept;
    ConsoleSurface(const ConsoleSurface &) = delete;
    ConsoleSurface &operator=(const ConsoleSurface &) = delete;
    ~ConsoleSurface();

    core::Size Size() const noexcept;
    bool IsOpen() const noexcept;
    GlassKind Glass() const noexcept;
    std::uint8_t Mosaics() const noexcept;
    std::expected<core::Point, box::error> Cursor() const noexcept;

    box::status Paint(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Paint(core::Point Origin, const Frame &FrameBuffer,
                      core::Rect Region) noexcept;
    box::status SetCursor(core::Point At) noexcept;
    box::status ScrollUp() noexcept;
    box::status Commit() noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit ConsoleSurface(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_CONSOLE_SURFACE_H
