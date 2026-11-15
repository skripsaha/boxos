#ifndef PAINTER_TUI_SURFACE_H
#define PAINTER_TUI_SURFACE_H

#include <cstdint>
#include <expected>
#include <memory>

#include "painter/tui/stage.h"

namespace painter::tui {

enum class SurfaceRole {
    Overlay,
    Notice,
};

/* A process-owned retained layer. Its cells are composed above the base scene
 * while the owner is alive and disappear when Close() or destruction occurs. */
class Surface {
public:
    static std::expected<Surface, StageError> Open(std::int32_t LeastCols = 0,
                                                   std::int32_t LeastRows = 0,
                                                   SurfaceRole Role = SurfaceRole::Overlay) noexcept;

    Surface(Surface &&) noexcept;
    Surface &operator=(Surface &&) noexcept;
    Surface(const Surface &) = delete;
    Surface &operator=(const Surface &) = delete;
    ~Surface();

    core::Size Size() const noexcept;
    bool IsOpen() const noexcept;
    GlassKind Glass() const noexcept;
    std::uint8_t Mosaics() const noexcept;

    box::status Composite(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer,
                          core::Rect Region) noexcept;
    box::status Clear() noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Surface(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_SURFACE_H
