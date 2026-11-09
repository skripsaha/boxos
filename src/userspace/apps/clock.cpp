/* clock — a notice-layer clock at the upper-right corner, shown as the
 * person set it: `settings clock` says how, and a change is seen at once. */

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "box/clock.h"
#include "box/error.h"
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

BOX_SETTINGS("setting hours one-of 24 12 default 24 \"how the hours are counted\"\n"
             "setting seconds yes-or-no default yes \"whether the seconds are shown\"\n"
             "setting digits color default #E6E6E6 \"the colour of the digits\"\n"
             "setting colons color default #8C8C8C \"the colour of the colons\"\n"
             "setting background color default #262626 \"the colour behind the time\"\n");

namespace {

using PainterColor = painter::core::Color;
constexpr std::int32_t ClockWidth = 11;   /* the widest it is: 12:59:59 PM */

/* How the person set the clock to look. */
struct Look {
    bool         TwelveHours;
    bool         Seconds;
    PainterColor Digits;
    PainterColor Colons;
    PainterColor Background;
};

PainterColor Rgb(std::uint32_t Value)
{
    return PainterColor::Rgb(static_cast<std::uint8_t>(Value >> 16), static_cast<std::uint8_t>(Value >> 8),
                             static_cast<std::uint8_t>(Value));
}

Look ReadLook()
{
    return Look{box::settings::choice("hours") == "12", box::settings::yes("seconds"),
                Rgb(box::settings::color("digits")), Rgb(box::settings::color("colons")),
                Rgb(box::settings::color("background"))};
}

char Digit(std::int64_t Value)
{
    return static_cast<char>('0' + Value);
}

bool Draw(painter::tui::Surface &Screen, painter::tui::Frame &Canvas, const Look &Shown)
{
    BoxTime Now{};
    if (clock_boxtime_local(&Now) != OK) return true;

    std::int64_t Hour = Now.hour;
    bool         Afternoon = Hour >= 12;
    if (Shown.TwelveHours) {
        Hour %= 12;
        if (Hour == 0) Hour = 12;
    }
    char         Text[ClockWidth];
    std::int32_t Length = 0;
    Text[Length++] = Digit(Hour / 10);
    Text[Length++] = Digit(Hour % 10);
    Text[Length++] = ':';
    Text[Length++] = Digit(Now.minute / 10);
    Text[Length++] = Digit(Now.minute % 10);
    if (Shown.Seconds) {
        Text[Length++] = ':';
        Text[Length++] = Digit(Now.second / 10);
        Text[Length++] = Digit(Now.second % 10);
    }
    if (Shown.TwelveHours) {
        Text[Length++] = ' ';
        Text[Length++] = Afternoon ? 'P' : 'A';
        Text[Length++] = 'M';
    }

    /* The time keeps to the right edge; the cells it does not need stay clear. */
    Canvas.ClearTransparent();
    for (std::int32_t Col = 0; Col < Length; Col++) {
        const PainterColor Ink = Text[Col] == ':' ? Shown.Colons : Shown.Digits;
        Canvas.Put({ClockWidth - Length + Col, 0}, painter::tui::Cell{Text[Col], Ink, Shown.Background});
    }
    if (!Screen.Clear()) return false;
    return static_cast<bool>(Screen.Composite({Screen.Size().Width - ClockWidth, 0}, Canvas));
}

}  // namespace

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(ClockWidth, 1, painter::tui::SurfaceRole::Notice);
    if (!Opened) return 1;
    painter::tui::Surface Screen = std::move(*Opened);
    if (Screen.Size().Width < ClockWidth || Screen.Size().Height == 0) return 1;

    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({ClockWidth, 1});
    if (!Created) return 1;
    painter::tui::Frame Canvas = std::move(*Created);

    box::settings::watch Watch;
    Look                 Shown = ReadLook();
    auto                 Next  = std::chrono::steady_clock::now();
    for (;;) {
        if (Watch.changed()) Shown = ReadLook();
        if (!Draw(Screen, Canvas, Shown)) return 1;
        Next += std::chrono::milliseconds(100);
        std::this_thread::sleep_until(Next);
    }
}
