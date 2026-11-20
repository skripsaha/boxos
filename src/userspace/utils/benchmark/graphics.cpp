/* graphics.cpp — `benchmark graphics`: the engine, then the scenes, then what
 * they came to, said and saved. See benchmark.h. */

#include <expected>
#include <optional>

#include "benchmark.h"
#include "box/system.h"
#include "painter/core/cpu_path.h"
#include "painter/gui/area.h"
#include "painter/tui/keyboard.h"

namespace benchmark {

namespace {

constexpr const char *Saved = "benchmark-graphics.log";

}  // namespace

int Graphics()
{
    Report        Out;
    system_info_t Info{};
    const unsigned Cores = sysinfo(&Info) == 0 ? Info.cpu_app_cores : 0;
    Out.Say("benchmark graphics: %u App Core(s); the engine works %s, the fastest way this processor offers", Cores,
            painter::core::CpuPathName(painter::core::BestCpuPath()));
    Engine(Out);
    Out.Print();

    Tally Got;
    {
        std::expected<painter::gui::Area, box::error> Opened = painter::gui::Area::Open(painter::gui::Placement::Whole);
        if (!Opened || Opened->On() != painter::gui::Glass::Pixels) {
            Out.Say("benchmark graphics: the scenes are drawn in a framebuffer's pixels; this screen is a text page — not measured");
            Out.Print();
            (void)Out.Save(Saved);
            return 1;
        }
        std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
        if (!Keys) {
            Out.Say("benchmark graphics: the keyboard is not to be had, and nothing could stop the scenes — not measured");
            Out.Print();
            return 1;
        }
        Out.Say("benchmark graphics: the screen %.0fx%.0f points at %.2f pixels a point", static_cast<double>(Opened->Size().X),
                static_cast<double>(Opened->Size().Y), static_cast<double>(Opened->Scale()));
        Got = Scenes(*Opened, *Keys, Out);
    }
    const int Measured = Got.Smooth + Got.Uneven + Got.Slideshow;
    Out.Say("benchmark graphics: %d of %d scenes measured — %d smooth, %d uneven, %d a slideshow; %u change(s) refused%s", Measured,
            Got.Count, Got.Smooth, Got.Uneven, Got.Slideshow, Got.Refused, Got.Stopped ? "; stopped before the end" : "");
    Out.Print();
    (void)Out.Save(Saved);
    return Got.Stopped ? 1 : 0;
}

}  // namespace benchmark
