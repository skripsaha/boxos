/* clock — the time in the screen's top-right corner, a notice of its own
 * (panel.h), shown as the person set it: `settings clock` says how, and a
 * change is seen by the next second. It wakes as each second turns, and says
 * the display only what changed — the time's figures, which the display
 * fades into the next while it sleeps. */

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "box/alarm.h"
#include "box/clock.h"
#include "box/cxx/settings.h"
#include "box/error.h"
#include "box/life.h"
#include "box/settings.h"
#include "painter/core/color.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"

#include "panel.h"

BOX_LIFE("stays");

BOX_SETTINGS("hours one-of 24 12 default 24 \"how the hours are counted\"\n"
             "seconds on-or-off default on \"whether the seconds are shown\"\n"
             "digits color default #E6E6E6 \"the color of the digits\"\n"
             "colons color default #8C8C8C \"the color of the colons\"\n"
             "background color default #262626 \"the color behind the time\"\n");

using namespace std::chrono_literals;

namespace {

namespace gui = painter::gui;
using PainterColor = painter::core::Color;

/* How the person set the clock to look. */
struct Look {
    bool         TwelveHours = false;
    bool         Seconds     = true;
    PainterColor Digits, Colons, Background;
};

PainterColor Rgb(std::uint32_t Value)
{
    return PainterColor::Rgb(static_cast<std::uint8_t>(Value >> 16), static_cast<std::uint8_t>(Value >> 8), static_cast<std::uint8_t>(Value));
}

Look ReadLook()
{
    return Look{box::settings::choice("hours") == "12", box::settings::on("seconds"), Rgb(box::settings::color("digits")),
                Rgb(box::settings::color("colons")), Rgb(box::settings::color("background"))};
}

/* The time as it is said: hours, minutes, seconds when they are shown, AM or
 * PM when the hours are twelve. */
std::string TimeOf(const Look &Shown, const BoxTime &Now)
{
    std::int64_t Hour = Now.hour;
    const bool   Afternoon = Hour >= 12;
    if (Shown.TwelveHours) {
        Hour %= 12;
        if (Hour == 0) Hour = 12;
    }
    const auto Two = [](std::int64_t Value) { return std::string{static_cast<char>('0' + Value / 10), static_cast<char>('0' + Value % 10)}; };
    std::string Said = Two(Hour) + ":" + Two(Now.minute);
    if (Shown.Seconds) Said += ":" + Two(Now.second);
    if (Shown.TwelveHours) Said += Afternoon ? " PM" : " AM";
    return Said;
}

/* The notice: the time in its figures' colour, its colons in theirs, on its
 * background, kept to the corner. */
void Show(const gui::Area &Screen, gui::Shape &Plate, gui::Text &Time, const Look &Shown, const std::string &Said)
{
    gui::TextStyle Figures;
    Figures.Color = Shown.Digits;
    gui::TextStyle Colon;
    Colon.Color = Shown.Colons;
    std::vector<gui::Span> Colons;
    for (std::uint32_t At = 0; At < Said.size(); At++)
        if (Said[At] == ':') Colons.push_back(gui::Span{At, At + 1, Colon});
    const float Wide = gui::Measure(Screen, Said, panel::Words).X + 2 * panel::Padding;
    const float High = panel::Height(Screen);
    const float Left = Screen.Size().X - panel::Margin - Wide;
    Plate.SetPlace({Left, panel::Margin}).SetSize({Wide, High}).SetFill(gui::Fill::Of(Shown.Background));
    Time.SetText(Said).SetStyle(Figures).SetSpans(Colons).SetPlace({Left + panel::Padding, panel::Margin + panel::Inset});
}

/* The ClockBoard's moment the next second turns at, in µs. */
std::uint64_t NextSecond()
{
    const std::uint64_t IntoSecond = clock_unix_now_ns() / 1000u % 1000000u;
    return clock_uptime_us() + (1000000u - IntoSecond) + 1u;
}

}  // namespace

int main()
{
    std::expected<gui::Area, box::error> Opened = panel::Open();
    if (!Opened) return 1;
    gui::Area &Screen = *Opened;
    gui::Shape Plate  = Screen.Rectangle();
    Plate.SetCorners(painter::core::Corners::All(panel::Round));
    gui::Text Time = Screen.Text("", panel::Words);

    box::settings::watch Watch;
    Look                 Shown = ReadLook();
    /* The notice comes in out of nothing; after, a minute's figures fade into
     * the next, and a second's are simply the next — a clock at rest asks the
     * display for one frame a second, not a fade's every second. */
    Plate.SetOpacity(0);
    Time.SetOpacity(0);
    std::int64_t Minute = -1;
    for (bool First = true;; First = false) {
        if (Watch.changed()) Shown = ReadLook();
        BoxTime Now{};
        if (clock_boxtime_local(&Now) == OK) {
            const std::int64_t This = std::int64_t{Now.hour} * 60 + Now.minute;
            if (First || This == Minute) Show(Screen, Plate, Time, Shown, TimeOf(Shown, Now));
            else Screen.Animate(gui::Animation::InOut(220ms), [&] { Show(Screen, Plate, Time, Shown, TimeOf(Shown, Now)); });
            Minute = This;
        }
        if (First) Screen.Animate(gui::Animation::Out(600ms), [&] {
            Plate.SetOpacity(1);
            Time.SetOpacity(1);
        });
        (void)Screen.Present();
        (void)alarm_set_us(NextSecond());
        /* The second turned — or the screen changed, and the corner with it. */
        for (;;) {
            const gui::Event Heard = Screen.Wait();
            if (Heard.What == gui::Event::Kind::Alarm || Heard.What == gui::Event::Kind::Area) break;
        }
    }
}
