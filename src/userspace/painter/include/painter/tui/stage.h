#ifndef PAINTER_TUI_STAGE_H
#define PAINTER_TUI_STAGE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "box/cxx/error.h"

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace painter::tui {

namespace Modifiers {
inline constexpr std::uint8_t Shift     = 0x01u;
inline constexpr std::uint8_t Ctrl      = 0x02u;
inline constexpr std::uint8_t Alt       = 0x04u;
inline constexpr std::uint8_t Extended  = 0x08u;
}  // namespace Modifiers

namespace KeyCodes {
inline constexpr char Escape    = 27;
inline constexpr char Backspace = '\b';
inline constexpr char Enter     = '\n';
inline constexpr char Tab       = '\t';
inline constexpr std::uint8_t Home     = 0x47;
inline constexpr std::uint8_t Up       = 0x48;
inline constexpr std::uint8_t PageUp   = 0x49;
inline constexpr std::uint8_t Left     = 0x4B;
inline constexpr std::uint8_t Right    = 0x4D;
inline constexpr std::uint8_t End      = 0x4F;
inline constexpr std::uint8_t Down     = 0x50;
inline constexpr std::uint8_t PageDown = 0x51;
inline constexpr std::uint8_t Insert   = 0x52;
inline constexpr std::uint8_t Delete   = 0x53;
}  // namespace KeyCodes

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

struct KeyEvent {
    char        Ascii    = 0;
    std::uint8_t Scancode = 0;
    std::uint8_t Mods     = 0;

    bool HasCharacter() const noexcept { return Ascii != 0; }
    bool IsExtended() const noexcept { return (Mods & Modifiers::Extended) != 0; }
    bool IsShift() const noexcept { return (Mods & Modifiers::Shift) != 0; }
    bool IsCtrl() const noexcept { return (Mods & Modifiers::Ctrl) != 0; }
    bool IsAlt() const noexcept { return (Mods & Modifiers::Alt) != 0; }
};

enum class EventKind { Key, Quiet, Gone };

struct Event {
    EventKind Kind = EventKind::Gone;
    KeyEvent  Key;
    box::error Cause;
};

class Stage {
public:
    static std::expected<Stage, StageError> Open(std::string_view Name, std::int32_t LeastCols,
                                                  std::int32_t LeastRows) noexcept;

    Stage(Stage &&Other) noexcept;
    Stage &operator=(Stage &&Other) noexcept;
    Stage(const Stage &)            = delete;
    Stage &operator=(const Stage &) = delete;
    ~Stage();

    core::Size Size() const noexcept;
    bool       IsOpen() const noexcept;

    Frame &FrameBuffer() noexcept;
    const Frame &FrameBuffer() const noexcept;

    box::status Present() noexcept;
    Event       Wait(std::uint32_t TimeoutMs = 0) noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Stage(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_STAGE_H
