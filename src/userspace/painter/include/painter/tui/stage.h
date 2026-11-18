#ifndef PAINTER_TUI_STAGE_H
#define PAINTER_TUI_STAGE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "box/cxx/error.h"
#include "box/keyboard.h"

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/key.h"

namespace painter::tui {

enum class StageErrorKind {
    DimensionsUnavailable,
    NoConsole,
    TooSmall,
    FrameUnavailable,
    KeyboardUnavailable,
    InvalidState,
};

struct StageError {
    StageErrorKind Kind = StageErrorKind::InvalidState;
    box::error     Cause;
    core::Size     Actual;
    core::Size     Required;

    std::string Describe(std::string_view Subject) const;
};

// EndAsked: the program is asked to finish — Ctrl+Alt+End over it, said by
// its keeper through the display. Only a program that said it answers that
// itself (HearEnd) hears it; any other hears Gone, and ends as it ends when
// there is nothing left to hear.
enum class EventKind { Key, Quiet, Gone, EndAsked };

struct Event {
    EventKind Kind = EventKind::Gone;
    KeyEvent  Key;
    box::error Cause;
};

class Stage {
public:
    static std::expected<Stage, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows) noexcept;

    Stage(Stage &&Other) noexcept;
    Stage &operator=(Stage &&Other) noexcept;
    Stage(const Stage &)            = delete;
    Stage &operator=(const Stage &) = delete;
    ~Stage();

    core::Size Size() const noexcept;
    bool       IsOpen() const noexcept;
    // Whether the glass is a text page or pixels, as the display holds it.
    GlassKind  Glass() const noexcept;
    // The grids finer than the halves it draws exactly (box/items.h ITEM_MOSAIC_*).
    std::uint8_t Mosaics() const noexcept;

    Frame &FrameBuffer() noexcept;
    const Frame &FrameBuffer() const noexcept;

    box::status Present() noexcept;
    Event       Poll() noexcept;
    Event       Wait(std::uint32_t TimeoutMs = 0) noexcept;
    // The program answers a request to finish itself: EndAsked, not Gone.
    void        HearEnd() noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Stage(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_STAGE_H
