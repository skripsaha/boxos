/* pulse — the machine's pulse, a notice of the panel (panel.h) beside the
 * clock: the processor's load these last beats as bars, each as tall as the
 * load and as warm, and the last as a figure; the memory in use, where the
 * screen is wide. Every half second it looks, and says the display only what
 * changed, in time: the bars grow and shrink to it and the figures fade. On a
 * text page each bar is shown by its caption — a block as tall as the load. */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "box/alarm.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/life.h"
#include "box/system.h"
#include "painter/core/color.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"

#include "panel.h"

BOX_LIFE("stays");

using namespace std::chrono_literals;

namespace {

namespace gui = painter::gui;
using PainterColor = painter::core::Color;

constexpr std::uint64_t BeatMs      = 500;
constexpr std::size_t   HistoryMost = 10;
constexpr float         BarWide     = 3, BarGap = 1;   // points
constexpr float         WideScreen  = 960;             // points: the memory shown from here

constexpr PainterColor Ink    = PainterColor::Rgb(0xE6, 0xE6, 0xE6);
constexpr PainterColor Ground = PainterColor::Rgb(0x26, 0x26, 0x26);
constexpr PainterColor Quiet  = PainterColor::Rgb(0x8C, 0x8C, 0x8C);
constexpr PainterColor Calm   = PainterColor::Rgb(0x50, 0xD0, 0x60);
constexpr PainterColor Busy   = painter::core::Colors::Amber;
constexpr PainterColor Hot    = PainterColor::Rgb(0xFF, 0x50, 0x50);

PainterColor Warmth(int Percent) { return Percent >= 85 ? Hot : Percent >= 50 ? Busy : Calm; }

/* A bar on a text page: a block as tall as the load. */
const char *Block(int Percent) { return Percent < 35 ? "_" : Percent < 70 ? "\xE2\x96\x84" : "\xE2\x96\x88"; }

/* What the machine does: the processor's load each beat, the memory used. */
class Machine {
public:
    void Look()
    {
        system_resources_t Now{};
        if (system_resources(&Now) == 0) {
            if (Seen && Now.cpu_total_tsc > Last.cpu_total_tsc && Now.cpu_busy_tsc >= Last.cpu_busy_tsc) {
                const std::uint64_t Total = Now.cpu_total_tsc - Last.cpu_total_tsc;
                const std::uint64_t Spent = Now.cpu_busy_tsc - Last.cpu_busy_tsc;
                if (History.size() == HistoryMost) History.erase(History.begin());
                History.push_back(static_cast<int>(std::min<std::uint64_t>(100, Spent * 100 / Total)));
            }
            Last = Now;
            Seen = true;
        }
        system_info_t Info{};
        if (sysinfo(&Info) == 0 && Info.total_memory > 0)
            Memory = static_cast<int>(std::min<std::uint64_t>(100, Info.used_memory * 100 / Info.total_memory));
    }
    const std::vector<int> &Loads() const noexcept { return History; }
    int                     Used() const noexcept { return Memory; }

private:
    system_resources_t Last{};
    bool               Seen = false;
    std::vector<int>   History;
    int                Memory = -1;
};

/* The notice's items, made once and told each beat what they show. */
struct Notice {
    explicit Notice(gui::Area &Screen)
        : Plate(Screen.Rectangle()), Cpu(Screen.Text("cpu", panel::Words)), Load(Screen.Text("", panel::Words)),
          Mem(Screen.Text("mem", panel::Words)), Used(Screen.Text("", panel::Words))
    {
        Plate.SetCorners(painter::core::Corners::All(panel::Round)).SetFill(gui::Fill::Of(Ground));
        for (gui::Shape &Bar : Bars) Bar = Screen.Rectangle();
        gui::TextStyle Faint;
        Faint.Color = Quiet;
        Cpu.SetStyle(Faint);
        Mem.SetStyle(Faint);
    }
    gui::Shape                          Plate;
    gui::Text                           Cpu, Load, Mem, Used;
    std::array<gui::Shape, HistoryMost> Bars;
};

gui::TextStyle Coloured(PainterColor Of)
{
    gui::TextStyle Made;
    Made.Color = Of;
    return Made;
}

/* The notice laid out right to left from where the clock's corner ends. */
void Show(const gui::Area &Screen, Notice &Shown, const Machine &Now)
{
    const float              High  = panel::Height(Screen);
    const float              Top   = panel::Margin;
    const float              Right = Screen.Size().X - panel::Margin - panel::ClockWidth(Screen) - panel::Gap;
    const std::vector<int>  &Loads = Now.Loads();
    const bool               Wide  = Screen.Size().X >= WideScreen && Now.Used() >= 0;
    const auto               Width = [&](const std::string &Said) { return gui::Measure(Screen, Said, panel::Words).X; };
    const std::string        Figure = Loads.empty() ? "--%" : std::format("{:>3}%", Loads.back());
    const std::string        Memory = Wide ? std::format("{:>3}%", Now.Used()) : std::string();

    /* Right to left: the memory's figure and word, the load's figure, the
     * bars, the processor's word. */
    float At = Right - panel::Padding;
    if (Wide) {
        At -= Width("100%");
        Shown.Used.SetText(Memory).SetStyle(Coloured(Now.Used() >= 85 ? Hot : Ink)).SetPlace({At, Top + panel::Inset}).Show();
        At -= Width("mem") + 6;
        Shown.Mem.SetPlace({At, Top + panel::Inset}).Show();
        At -= 10;
    } else {
        Shown.Used.Hide();
        Shown.Mem.Hide();
    }
    At -= Width("100%");
    const PainterColor Last = Loads.empty() ? Quiet : Warmth(Loads.back()) == Calm ? Ink : Warmth(Loads.back());
    Shown.Load.SetText(Figure).SetStyle(Coloured(Last)).SetPlace({At, Top + panel::Inset});
    /* On a text page a bar is its caption, a cell each in the words' row. */
    const bool  Page  = Screen.On() == gui::Glass::Text;
    const float Pitch = Page ? static_cast<float>(ITEM_PAGE_CELL_WIDE) : BarWide + BarGap;
    At -= 6 + HistoryMost * Pitch;
    const float Floor = Top + High - panel::Inset - 1, Tallest = High - 2 * panel::Inset - 2;
    for (std::size_t Bar = 0; Bar < HistoryMost; Bar++) {
        /* The newest at the right; a beat not yet seen shows nothing. */
        const std::size_t Back = HistoryMost - 1 - Bar;
        gui::Shape       &One  = Shown.Bars[Bar];
        const float       X    = At + static_cast<float>(Bar) * Pitch;
        if (Back >= Loads.size()) {
            One.Hide();
            continue;
        }
        const int   Percent = Loads[Loads.size() - 1 - Back];
        const float Tall    = std::max(1.0f, Tallest * static_cast<float>(Percent) / 100.0f);
        One.SetPlace({X, Page ? Top + panel::Inset : Floor - Tall})
            .SetSize({BarWide, Tall})
            .SetFill(gui::Fill::Of(Warmth(Percent)))
            .SetCaption(Block(Percent))
            .Show();
    }
    At -= Width("cpu") + 6;
    Shown.Cpu.SetPlace({At, Top + panel::Inset});
    At -= panel::Padding;
    Shown.Plate.SetPlace({At, Top}).SetSize({Right - At, High});
}

}  // namespace

int main()
{
    std::expected<gui::Area, box::error> Opened = panel::Open();
    if (!Opened) return 1;
    gui::Area &Screen = *Opened;
    Notice     Shown(Screen);
    Machine    Now;
    /* The notice comes in out of nothing; after, each beat the bars grow and
     * shrink to the load and the figures fade into the next, the display
     * playing it while pulse sleeps. */
    const auto Seen = [&Shown](float Opacity) {
        Shown.Plate.SetOpacity(Opacity);
        Shown.Cpu.SetOpacity(Opacity);
        Shown.Load.SetOpacity(Opacity);
        Shown.Mem.SetOpacity(Opacity);
        Shown.Used.SetOpacity(Opacity);
    };
    Seen(0);
    for (std::uint64_t Due = clock_uptime_ms(), Beat = 0;; Beat++) {
        Now.Look();
        if (Beat == 0) {
            Show(Screen, Shown, Now);
            Screen.Animate(gui::Animation::Out(600ms), [&] { Seen(1); });
        } else {
            Screen.Animate(gui::Animation::Out(BeatMs * 3 / 4 * 1ms), [&] { Show(Screen, Shown, Now); });
        }
        (void)Screen.Present();
        /* The next beat: a beat behind is not made up, only the next kept. */
        Due = std::max(Due + BeatMs, clock_uptime_ms() + 1);
        (void)alarm_set(Due);
        /* The beat came — or the screen changed, and the corner with it. */
        for (;;) {
            const gui::Event Heard = Screen.Wait();
            if (Heard.What == gui::Event::Kind::Alarm || Heard.What == gui::Event::Kind::Area) break;
        }
    }
}
