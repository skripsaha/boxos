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
 * then it spouts, and its tail leaves bubbles. Its speed, size, colours, outline,
 * spout and bubbles are the person's (settings whale), changed while it swims.
 * What it is made of: swim (the course, the body and the drops, which know
 * nothing of the glass), figure (the colour at any pixel of it) and picture
 * (the cells laid over the glass).
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
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

#include "picture.h"
#include "swim.h"

BOX_FORM("");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast the whale swims and lives\"\n"
             "size number from 50 to 200 percent default 100 \"how large the whale is\"\n"
             "back color default #1E4A98 \"the whale's back: its fins and flukes are shades of it\"\n"
             "belly color default #C4DEF2 \"the whale's belly: its flank leans between the back and this\"\n"
             "outline on-or-off default on \"a dark line round the whale, on its own edge\"\n"
             "smooth on-or-off default off \"the whale's edge fades into what lies beneath; off keeps every edge pixel whole or none\"\n"
             "spout on-or-off default on \"now and then it blows a spout\"\n"
             "bubbles on-or-off default on \"its tail leaves bubbles\"\n");

namespace {

constexpr std::uint32_t FrameMs = 50;

/* What the person set, read again whenever the file is saved. */
struct Look {
    float                Speed;
    float                Size;
    painter::core::Color Back, Belly;
    bool                 Outline, Smooth, Spout, Bubbles;
};

Look ReadLook()
{
    using box::settings;
    return Look{.Speed   = static_cast<float>(settings::number("speed")) / 100.0f,
                .Size    = static_cast<float>(settings::number("size")) / 100.0f,
                .Back    = painter::core::Color::FromRaw(settings::color("back")),
                .Belly   = painter::core::Color::FromRaw(settings::color("belly")),
                .Outline = settings::on("outline"),
                .Smooth  = settings::on("smooth"),
                .Spout   = settings::on("spout"),
                .Bubbles = settings::on("bubbles")};
}

void Apply(const Look &Set, whale::Swimmer &Whale, whale::Picture &Picture)
{
    Whale.Tune(Set.Speed, Set.Spout, Set.Bubbles);
    Picture.Dress(whale::Dress(Set.Back, Set.Belly), Set.Outline, Set.Smooth);
}

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

    Look           Set = ReadLook();
    whale::Swimmer Whale;
    whale::Picture Picture(Glass);
    Apply(Set, Whale, Picture);
    Whale.Begin(Glass.Width, Glass.Height * 2, Set.Size);
    box::settings::watch Watch;

    // Painted until it can be no more — the display gone, or the whale
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            Apply(Set, Whale, Picture);
            Whale.Fit(Set.Size);
        }
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
