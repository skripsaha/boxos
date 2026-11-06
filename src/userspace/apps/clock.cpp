/* clock — a notice-layer clock at the upper-right corner. */

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "box/clock.h"
#include "box/error.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

namespace {

using PainterColor = painter::core::Color;
constexpr std::int32_t ClockWidth = 8;
constexpr PainterColor Foreground = PainterColor::Rgb(0xE6, 0xE6, 0xE6);
constexpr PainterColor Separator = PainterColor::Rgb(0x8C, 0x8C, 0x8C);
constexpr PainterColor Background = PainterColor::Rgb(0x26, 0x26, 0x26);

char Digit(std::int64_t Value)
{
    return static_cast<char>('0' + Value);
}

bool Draw(painter::tui::Surface &Screen, painter::tui::Frame &Canvas)
{
    BoxTime Now{};
    if (clock_boxtime_local(&Now) != OK) return true;

    const char Text[ClockWidth] = {
        Digit(Now.hour / 10), Digit(Now.hour % 10), ':',
        Digit(Now.minute / 10), Digit(Now.minute % 10), ':',
        Digit(Now.second / 10), Digit(Now.second % 10)};
    Canvas.ClearTransparent();
    for (std::int32_t Col = 0; Col < ClockWidth; Col++) {
        const PainterColor Ink = Text[Col] == ':' ? Separator : Foreground;
        Canvas.Put({Col, 0}, painter::tui::Cell{Text[Col], Ink, Background});
    }
    if (!Screen.Clear()) return false;
    return static_cast<bool>(Screen.Composite({Screen.Size().Width - ClockWidth, 0}, Canvas));
}

}  // namespace

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open("clock", ClockWidth, 1, painter::tui::SurfaceRole::Notice);
    if (!Opened) return 1;
    painter::tui::Surface Screen = std::move(*Opened);
    if (Screen.Size().Width < ClockWidth || Screen.Size().Height == 0) return 1;

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({ClockWidth, 1});
    if (!Created) return 1;
    painter::tui::Frame Canvas = std::move(*Created);

    auto Next = std::chrono::steady_clock::now();
    for (;;) {
        if (!Draw(Screen, Canvas)) return 1;
        Next += std::chrono::milliseconds(100);
        std::this_thread::sleep_until(Next);
    }
}
