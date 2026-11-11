/* sand — a sandbox of falling things, on the half-block pixels of the glass.
 *
 *   sand                   an empty box
 *   sand beach             begun in a scene: empty, beach, volcano or garden
 *
 * Sand falls and piles into slopes; water runs and levels out; oil floats on
 * it and burns hot; fire climbs, catches what burns and dies into smoke; lava
 * melts sand to glass and turns water to steam, which cools and falls as
 * water; seeds take root beside water and plants drink it and grow. ctrl+?
 * shows every key; settings sand sets the speed, how far water runs, which way
 * things fall, the colors of every material, and the keys. What it is made of:
 * materials (what each thing is), world with rules and chemistry (how they
 * move and act), scenes, look (the settings), sandbox (the brush and the keys)
 * and paint (the picture). Only paint and this part know the screen. */

#include <algorithm>
#include <new>
#include <string>
#include <string_view>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/cxx/settings.h"
#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "paint.h"
#include "sandbox.h"

BOX_FORM("takes optional name \"the scene it begins in: empty, beach, volcano or garden\"\n");

namespace {

namespace tui = painter::tui;

constexpr std::int32_t LeastCols = 48;
constexpr std::int32_t LeastRows = 18;
// Frames come by the clock; the sand's steps are the sandbox's, owed by time.
constexpr std::uint64_t FrameUs   = 33333;
// While paused nothing moves: the picture is made again only to keep up with the settings.
constexpr std::uint64_t PausedUs  = 250000;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

// False to leave: the screen is gone, the program was asked to finish, or a key said so.
bool Hear(tui::Stage &Screen, sand::Sandbox &Box, std::uint64_t Until)
{
    for (;;) {
        const std::uint64_t Now = clock_uptime_us();
        if (Now >= Until) return true;
        const tui::Event Heard = Screen.Wait(static_cast<std::uint32_t>((Until - Now + 999) / 1000));
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return false;
        if (Heard.Kind != tui::EventKind::Key) continue;
        if (!Box.OnKey(Heard.Key)) return false;
        if (Box.Paused) return true;
    }
}

}  // namespace

int main()
{
    try {
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        const std::string_view Name  = Count > 0 ? Words[0] : "empty";
        const sand::Scene     *Begin = sand::FindScene(Name);
        if (!Begin) {
            Say("There is no scene called " + std::string(Name) + ": " + sand::SceneNames() + ".");
            return 1;
        }
        std::expected<tui::Stage, tui::StageError> Opened = tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("sand"));
            return 1;
        }
        tui::Stage Screen = std::move(*Opened);
        Screen.HearEnd();

        const painter::core::Size Glass = Screen.Size();
        sand::Sandbox             Box(*Begin, cpu_rdtsc() ^ clock_uptime_us());
        Box.Open(Glass.Width, (Glass.Height - 1) * 2, clock_uptime_us());
        sand::Picture      Picture;
        box::settings::watch Watch;
        bool               Painted = true;
        for (;;) {
            if (Watch.changed()) Box.Reread();
            const std::uint64_t Now = clock_uptime_us();
            Box.Advance(Now);
            Picture.Draw(Screen, Box);
            if (!(Painted = static_cast<bool>(Screen.Present()))) break;
            if (!Hear(Screen, Box, Now + (Box.Paused ? PausedUs : FrameUs))) break;
        }
        (void)Screen.Close();
        if (!Painted) Say("The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a sandbox the size of this screen.");
        return 1;
    }
}
