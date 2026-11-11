/* duppo — Duppo, the bubble with the big eye, floating about the screen.
 *
 *   duppo                   it starts at once and drifts on its own
 *   finish duppo            it is gone
 *
 * It lives on by itself, as the whale and the clock do (box/life.h stays):
 * whoever starts it gets the prompt back at once, and it floats over whatever
 * the screen shows without taking a key — a notice, not a window.
 *
 * It is drawn as the logo is (duppo-bubble.svg: a glossy body, an eye of a
 * pale ring round a pupil, and three little bubbles), every pixel shaded by
 * the logo's own gradients. It drifts between places on a soft spring and
 * bobs as it goes; its body gives like jelly when its way changes; it turns in
 * space about all three axes, so its eye goes round the ball and the little
 * bubbles pass behind it and before it; the bubbles keep close on springs of
 * their own and swing a little by themselves; the eye looks where it goes,
 * glances about while it rests, and blinks.
 * What it is made of: flight (the body's way), spin (its turn), face (the
 * eye), orbit (the bubbles), logo (the colour at any point of the logo) and
 * picture (the cells laid over the glass).
 *
 * Only the cells Duppo stands on are painted; every other cell is left
 * transparent, so the screen goes on being used beneath it. */

#include <algorithm>
#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <thread>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

#include "face.h"
#include "flight.h"
#include "orbit.h"
#include "picture.h"
#include "spin.h"

BOX_FORM("");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast Duppo drifts and lives\"\n"
             "size number from 40 to 250 percent default 100 \"how large Duppo is\"\n"
             "body color default #E978A0 \"the bubble's pink: its deep plum and the lip round the eye are shades of it\"\n"
             "ring color default #FFE2F0 \"the pale ring round the pupil: its shaded edge follows it\"\n"
             "pupil color default #000000 \"the pupil: its glow and its rim are lighter shades of it\"\n"
             "satellites number from 0 to 3 default 3 \"how many little bubbles keep close to Duppo; 0 leaves the bubble alone\"\n"
             "blink number from 20 to 400 percent or off default 100 \"how often Duppo blinks; off keeps the eye open\"\n");

namespace {

constexpr std::uint32_t FrameMs = 40;

/* How large Duppo is at the usual size: the glass units one unit of the logo takes. */
constexpr float UsualSize = 0.8f;

/* What the person set, read again whenever the file is saved. */
struct Look {
    float                Speed;
    float                Size;
    painter::core::Color Body, Ring, Pupil;
    int                  Satellites;
    float                Blink;
};

Look ReadLook()
{
    using box::settings;
    return Look{.Speed      = static_cast<float>(settings::number("speed")) / 100.0f,
                .Size       = static_cast<float>(settings::number("size")) / 100.0f,
                .Body       = painter::core::Color::FromRaw(settings::color("body")),
                .Ring       = painter::core::Color::FromRaw(settings::color("ring")),
                .Pupil      = painter::core::Color::FromRaw(settings::color("pupil")),
                .Satellites = static_cast<int>(settings::number("satellites")),
                .Blink      = static_cast<float>(settings::number("blink")) / 100.0f};
}

float SizeOf(const Look &Set, painter::core::Size Glass)
{
    return std::min(UsualSize * Set.Size, duppo::Flight::Largest(Glass));
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(30, 10, painter::tui::SurfaceRole::Notice);
    if (!Opened) {
        Say(Opened.error().Describe("duppo"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    painter::core::Size   Glass  = Screen.Size();

    std::expected<painter::tui::Frame, box::error> Created = painter::tui::Frame::Create(Glass);
    if (!Created) return 1;
    painter::tui::Frame Overlay = std::move(*Created);
    Overlay.ClearTransparent();
    Overlay.SetGlass(Screen.Glass());

    duppo::Random Dice;
    Dice.Seed(cpu_rdtsc());
    Look           Set = ReadLook();
    duppo::Flight  Body;
    duppo::Face    Eye;
    duppo::Spin    Turning;
    duppo::Orbit   Moons;
    duppo::Picture Picture(Glass);

    Body.Tune(Set.Speed);
    Eye.Tune(Set.Speed, Set.Blink);
    Turning.Tune(Set.Speed);
    Moons.Tune(Set.Speed, Set.Satellites);
    Picture.Dress(duppo::Palette::Make(Set.Body, Set.Ring, Set.Pupil));
    Body.Begin(Glass, SizeOf(Set, Glass), Dice);
    Eye.Begin(Glass);
    Moons.Begin(Body.Center, Body.Scale);
    box::settings::watch Watch;

    // Painted until it can be no more — the display gone, or Duppo
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            Body.Tune(Set.Speed);
            Eye.Tune(Set.Speed, Set.Blink);
            Turning.Tune(Set.Speed);
            Moons.Tune(Set.Speed, Set.Satellites);
            Picture.Dress(duppo::Palette::Make(Set.Body, Set.Ring, Set.Pupil));
            Body.Fit(SizeOf(Set, Glass));
        }
        std::uint64_t Now = clock_uptime_us();
        float Seconds     = std::min(0.25f, static_cast<float>(Now - Last) / 1e6f);
        Last              = Now;
        Body.Step(Seconds, Dice);
        Eye.Step(Seconds, Dice, Body);
        Turning.Step(Seconds, Dice, Body);
        Moons.Step(Seconds, Body.Center, Body.Scale, Turning);
        painter::core::Rect Changed = Picture.Draw(Body, Eye, Turning, Moons, Overlay);
        if (!Changed.Empty() && !Screen.Composite({Changed.X, Changed.Y}, Overlay, Changed)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(FrameMs));
    }
    (void)Screen.Close();
    return 1;
}
