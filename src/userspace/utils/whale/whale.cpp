/* whale — a blue whale swimming through the air over the screen.
 *
 *   whale                   it starts at once and swims on its own
 *   finish whale            it is gone
 *
 * It lives on by itself, as the clock and the eyes do (box/life.h stays):
 * whoever starts it gets the prompt back at once, and it swims over whatever
 * the screen shows without taking a key — a notice, not a window.
 *
 * The whale wanders on a slow course and turns away from the edges in wide
 * arcs, rolling over as it comes about; its body follows its head's path with
 * a wave running down it to the beat of the tail, and its fins swing. Now and
 * then it spouts, and its tail leaves bubbles. What it is made of: swim (the
 * course, the body and the drops, which know nothing of the glass), figure (the
 * colour at any pixel of it) and picture (the cells laid over the glass).
 *
 * Only the cells the whale stands on are painted; every other cell is left
 * transparent, so the screen goes on being used beneath it. */

#include <algorithm>
#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <thread>

#include "box/clock.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"

#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

#include "picture.h"
#include "swim.h"

BOX_FORM("");
BOX_LIFE("stays");

namespace {

constexpr std::uint32_t FrameMs = 50;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(30, 10, painter::tui::SurfaceRole::Notice);
    if (!Opened) {
        Say(Opened.error().Describe("whale"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    painter::core::Size   Glass  = Screen.Size();

    std::expected<painter::tui::Frame, box::error> Created = painter::tui::Frame::Create(Glass);
    if (!Created) return 1;
    painter::tui::Frame Overlay = std::move(*Created);
    Overlay.ClearTransparent();
    Overlay.SetGlass(Screen.Glass());

    whale::Swimmer Whale;
    Whale.Begin(Glass.Width, Glass.Height * 2);
    whale::Picture Picture(Glass);

    // Painted until it can be no more — the display gone, or the whale
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        std::uint64_t Now = clock_uptime_us();
        Whale.Step(std::min(0.25f, static_cast<float>(Now - Last) / 1e6f));
        Last = Now;
        painter::core::Rect Changed = Picture.Draw(Whale, Overlay);
        if (!Changed.Empty() && !Screen.Composite({Changed.X, Changed.Y}, Overlay, Changed)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(FrameMs));
    }
    (void)Screen.Close();
    return 1;
}
