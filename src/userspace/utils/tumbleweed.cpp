/* tumbleweed — a moving window over a layered BoxOS wasteland. */

#include <algorithm>
#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <string_view>

#include "box/clock.h"
#include "box/form.h"
#include "box/print.h"

#include "tumbleweed.h"

BOX_FORM("");

namespace {

constexpr std::uint64_t FrameUs = 33333;
constexpr float MostStep = 0.05f;
constexpr float MostGap = 0.25f;
constexpr int LeastCols = 48;
constexpr int LeastRows = 16;
namespace KeyCodes = painter::tui::KeyCodes;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

bool OnKey(TumbleWorld &World, const painter::tui::KeyEvent &Key)
{
    if (Key.IsExtended()) {
        const int StepX = std::max(4, World.ViewWidth / 4);
        const int StepY = std::max(4, World.ViewHeight / 4);
        switch (Key.Scancode) {
        case KeyCodes::Left: World.Move(-StepX, 0); break;
        case KeyCodes::Right: World.Move(StepX, 0); break;
        case KeyCodes::Up: World.Move(0, -StepY); break;
        case KeyCodes::Down: World.Move(0, StepY); break;
        default: break;
        }
        return true;
    }
    switch (Key.Ascii) {
    case 's':
    case 'S': World.AddWeed(World.CameraX + World.ViewWidth * 0.5f); break;
    case 'c':
    case 'C': World.AddCactus(World.CameraX + World.ViewWidth * 0.5f); break;
    case 'b':
    case 'B': World.AddBird(World.CameraX + World.ViewWidth * 0.5f, World.CameraY + 12); break;
    case 'w':
    case 'W': World.CycleWeather(); break;
    case 'e':
    case 'E': World.CycleSeason(); break;
    case 'n':
    case 'N': World.CycleLight(); break;
    case 'h':
    case 'H':
    case '?': World.ShowKeys = !World.ShowKeys; break;
    case 'q':
    case 'Q':
    case KeyCodes::Escape: return false;
    default: break;
    }
    return true;
}

}

int main()
{
    try {
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("tumbleweed"));
            return 1;
        }
        painter::tui::Stage Screen = std::move(*Opened);
        TumbleWorld World(Screen.Size().Width, Screen.Size().Height);
        const std::uint64_t Began = clock_uptime_us();
        std::uint64_t Last = Began;
        bool Painted = true;
        for (;;) {
            const std::uint64_t Now = clock_uptime_us();
            const float Elapsed = std::min(MostGap, static_cast<float>(Now - Last) / 1e6f);
            for (float Left = Elapsed; Left > 0; Left -= MostStep) World.Step(std::min(MostStep, Left));
            Last = Now;

            TumblePaint(World, Screen.FrameBuffer());
            if (!Screen.SetCursor({World.ViewWidth - 1, World.ViewHeight / 2}) ||
                !(Painted = static_cast<bool>(Screen.Present())))
                break;

            const std::uint64_t Spent = clock_uptime_us() - Now;
            const painter::tui::Event Event = Screen.Wait(
                Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
            if (Event.Kind == painter::tui::EventKind::Gone) break;
            if (Event.Kind == painter::tui::EventKind::Key && !OnKey(World, Event.Key)) break;
        }
        (void)Screen.Close();
        Say(std::format("[TUMBLEWEED] {} object{} entered the wasteland after {} seconds", World.Spawned,
                        World.Spawned == 1 ? "" : "s", (clock_uptime_us() - Began) / 1000000));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a wasteland the size of this screen.");
        return 1;
    }
}
