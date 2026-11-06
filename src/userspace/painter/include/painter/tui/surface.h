#ifndef PAINTER_TUI_SURFACE_H
#define PAINTER_TUI_SURFACE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "painter/tui/stage.h"

namespace painter::tui {

/* A non-owning output surface for programs that paint a region and return to
 * the caller's console. Surface never listens for keys and never clears the
 * surrounding screen; Stage is the full-screen owner for interactive apps. */
class Surface {
public:
    static std::expected<Surface, StageError> Open(std::string_view Name, std::int32_t LeastCols = 0,
                                                    std::int32_t LeastRows = 0) noexcept;

    Surface(Surface &&) noexcept;
    Surface &operator=(Surface &&) noexcept;
    Surface(const Surface &)            = delete;
    Surface &operator=(const Surface &) = delete;
    ~Surface();

    core::Size Size() const noexcept;
    bool       IsOpen() const noexcept;

    std::expected<core::Point, box::error> Cursor() const noexcept;
    /* Paint writes every cell in the selected rectangle, including spaces.
     * Composite replaces the selected retained-layer rectangle; transparent
     * cells clear that layer and reveal the surface below. */
    box::status Paint(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Paint(core::Point Origin, const Frame &FrameBuffer, core::Rect Region) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer, core::Rect Region) noexcept;
    /* Clear this surface's retained layer without presenting an intermediate
     * frame. The next Paint/Composite publishes the replacement atomically
     * with respect to the display server. */
    box::status Clear() noexcept;
    box::status SetCursor(core::Point At) noexcept;
    box::status ScrollUp() noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Surface(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_SURFACE_H
