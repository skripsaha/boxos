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
 * On a framebuffer Duppo is drawn in the screen's own pixels (pixels): it comes
 * out of nothing on a spring, its canvas carried smoothly between frames by
 * the display, and now and then it says a word in a bubble beside itself
 * (words) — unless the person keeps it quiet. On a text page it is drawn in
 * cells (picture). Either way only what Duppo stands on is painted; all else
 * is left see-through, so the screen goes on being used beneath it. */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "box/alarm.h"
#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/cxx/settings.h"
#include "box/cxx/timing.h"

#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/frame.h"
#include "painter/tui/surface.h"

#include "face.h"
#include "flight.h"
#include "orbit.h"
#include "picture.h"
#include "pixels.h"
#include "spin.h"
#include "words.h"

BOX_FORM("");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast Duppo drifts and lives\"\n"
             "size number from 40 to 250 percent default 100 \"how large Duppo is\"\n"
             "body color default #E978A0 \"the bubble's pink: its deep plum and the lip round the eye are shades of it\"\n"
             "ring color default #FFE2F0 \"the pale ring round the pupil: its shaded edge follows it\"\n"
             "pupil color default #000000 \"the pupil: its glow and its rim are lighter shades of it\"\n"
             "satellites number from 0 to 3 default 3 \"how many little bubbles keep close to Duppo; 0 leaves the bubble alone\"\n"
             "blink number from 20 to 400 percent or off default 100 \"how often Duppo blinks; off keeps the eye open\"\n"
             "smooth on-or-off default on \"Duppo's edge fades into what lies beneath; off keeps every edge whole or none\"\n"
             "fps number from 5 to 60 frames default 25 \"how many frames a second Duppo is drawn; more is smoother, the drifting is as fast\"\n"
             "talk on-or-off default on \"Duppo says a word now and then, in a bubble beside itself; off keeps it quiet\"\n");

namespace {

/* How large Duppo is at the usual size: the glass units one unit of the logo takes. */
constexpr float UsualSize = 0.8f;

/* What the person set, read again whenever the file is saved. */
struct Look {
    float                Speed;
    float                Size;
    painter::core::Color Body, Ring, Pupil;
    int                  Satellites;
    float                Blink;
    bool                 Smooth;
    unsigned             Fps;
    bool                 Talk;
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
                .Blink      = static_cast<float>(settings::number("blink")) / 100.0f,
                .Smooth     = settings::on("smooth"),
                .Fps        = static_cast<unsigned>(settings::number("fps")),
                .Talk       = settings::on("talk")};
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

/* What Duppo lives by, tuned to what the person set. */
struct Life {
    duppo::Flight Body;
    duppo::Face   Eye;
    duppo::Spin   Turning;
    duppo::Orbit  Moons;

    void Tune(const Look &Set)
    {
        Body.Tune(Set.Speed);
        Eye.Tune(Set.Speed, Set.Blink);
        Turning.Tune(Set.Speed);
        Moons.Tune(Set.Speed, Set.Satellites);
    }
    void Begin(const Look &Set, painter::core::Size Glass, duppo::Random &Dice)
    {
        Body.Begin(Glass, SizeOf(Set, Glass), Dice);
        Eye.Begin(Glass);
        Moons.Begin(Body.Center, Body.Scale);
    }
    void Step(float Seconds, duppo::Random &Dice)
    {
        Body.Step(Seconds, Dice);
        Eye.Step(Seconds, Dice, Body);
        Turning.Step(Seconds, Dice, Body);
        Moons.Step(Seconds, Body.Center, Body.Scale, Turning);
    }
};

/* A framebuffer as Duppo lives on it: a glass unit eight pixels a point, so
 * Duppo is as large as in cells; the glass in the cells the flight counts. */
struct Glass {
    float               Unit;    // pixels a glass unit
    float               Scale;   // pixels a point
    painter::core::Size Cells;
};

Glass GlassOf(const painter::gui::Area &Screen)
{
    const float Scale = Screen.Scale(), Unit = 8.0f * Scale;
    return Glass{Unit, Scale,
                 painter::core::Size{static_cast<std::int32_t>(Screen.Size().X * Scale / Unit),
                                     static_cast<std::int32_t>(Screen.Size().Y * Scale / (2.0f * Unit))}};
}

/* Duppo in pixels, over everything and shown always, on a canvas of its own
 * the display carries between frames. */
int FloatOnPixels(painter::gui::Area Screen)
{
    namespace gui = painter::gui;
    using namespace std::chrono_literals;
    duppo::Random Dice;
    Dice.Seed(cpu_rdtsc());
    Look           Set = ReadLook();
    Glass          On  = GlassOf(Screen);
    Life           Duppo;
    duppo::Pixels  Painter;
    duppo::Words   Speech(Screen);
    const auto     Tune = [&] {
        Duppo.Tune(Set);
        Painter.Dress(duppo::Palette::Make(Set.Body, Set.Ring, Set.Pupil), Set.Smooth);
        Speech.Tune(Set.Talk, Set.Speed);
    };
    Tune();
    Duppo.Begin(Set, On.Cells, Dice);

    /* The canvas, made again whenever Duppo's size or the glass asks for
     * another; anchored at its middle, where Duppo is. */
    painter::core::Size        Page = duppo::Pixels::PageFor(Duppo.Body.Scale, On.Unit);
    std::optional<gui::Canvas> Board;
    painter::core::Rect        Drawn{};
    bool                       Jump = true;   // the next place said at once, not carried
    const auto Make = [&] {
        Board.emplace(Screen.Canvas(Page));
        Board->SetAnchor({static_cast<float>(Page.Width) / 2.0f / On.Scale, static_cast<float>(Page.Height) / 2.0f / On.Scale});
        Drawn = {};
        Jump  = true;
    };
    Make();
    /* It comes out of nothing, on a spring. */
    Board->SetScale({0, 0}).SetOpacity(0);
    Screen.Animate(gui::Animation::Spring(800ms, 0.4f), [&] { Board->SetScale({1, 1}); });
    Screen.Animate(gui::Animation::Out(400ms), [&] { Board->SetOpacity(1); });

    box::settings::watch Watch;
    std::uint64_t        Last = clock_uptime_us(), Next = clock_uptime_ms();
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            Tune();
            Duppo.Body.Fit(SizeOf(Set, On.Cells));
            const painter::core::Size Wanted = duppo::Pixels::PageFor(Duppo.Body.Scale, On.Unit);
            if (Wanted.Width != Page.Width || Wanted.Height != Page.Height) {
                Page = Wanted;
                Make();
            }
        }
        const std::uint64_t Now = clock_uptime_us();
        Duppo.Step(std::min(0.25f, static_cast<float>(Now - Last) / 1e6f), Dice);
        Last = Now;

        /* The canvas's corner a whole pixel, Duppo in its middle to the
         * pixel's fraction; only what it touched then and touches now drawn
         * and given. */
        const duppo::Vec          Middle = Duppo.Body.Center * On.Unit;
        const painter::core::Point Corner{static_cast<std::int32_t>(std::lround(Middle.X - static_cast<float>(Page.Width) / 2.0f)),
                                          static_cast<std::int32_t>(std::lround(Middle.Y - static_cast<float>(Page.Height) / 2.0f))};
        const duppo::Vec           From{static_cast<float>(Corner.X) / On.Unit, static_cast<float>(Corner.Y) / On.Unit};
        const painter::core::Rect  Touched = duppo::Pixels::Touched(Duppo.Body, Duppo.Moons, Page, From, On.Unit);
        const painter::core::Rect  Changed = Touched.Join(Drawn);
        if (!Changed.Empty())
            (void)Board->Draw([&](painter::core::Canvas &Into) {
                Painter.Draw(Duppo.Body, Duppo.Eye, Duppo.Turning, Duppo.Moons, Into, From, On.Unit, Changed);
            }, Changed);
        Drawn = Touched;

        /* Carried there by the display over the frame, so it glides between
         * frames; its words along with it. */
        const auto                  Frame = std::chrono::milliseconds(1000 / std::max(Set.Fps, 1u));
        const painter::core::PointF Place{static_cast<float>(Corner.X) / On.Scale, static_cast<float>(Corner.Y) / On.Scale};
        if (Jump) Board->SetPlace(Place);
        else Screen.Animate(gui::Animation::Linear(Frame), [&] { Board->SetPlace(Place); });
        Jump = false;
        const float     Radius = duppo::Logo::Body.Radius * Duppo.Body.Scale;
        const auto      Points = [&](float X, float Y) { return painter::core::PointF{X * On.Unit / On.Scale, Y * On.Unit / On.Scale}; };
        Speech.Step(clock_uptime_ms(), Points(Duppo.Body.Center.X - Radius * 0.7f, Duppo.Body.Center.Y - Radius * 0.75f),
                    Points(Duppo.Body.Center.X + Radius * 0.7f, Duppo.Body.Center.Y - Radius * 0.75f), Dice, Frame);
        (void)Screen.Present();

        /* The next frame — or the glass changed under it, and Duppo begins
         * again on it. */
        Next = std::max<std::uint64_t>(Next + static_cast<std::uint64_t>(Frame.count()), clock_uptime_ms() + 1);
        (void)alarm_set(Next);
        for (;;) {
            const gui::Event Heard = Screen.Wait();
            if (Heard.What == gui::Event::Kind::Alarm) break;
            if (Heard.What != gui::Event::Kind::Area) continue;
            const Glass Now2 = GlassOf(Screen);
            if (Now2.Cells.Width == On.Cells.Width && Now2.Cells.Height == On.Cells.Height && Now2.Unit == On.Unit) continue;
            On = Now2;
            Duppo.Begin(Set, On.Cells, Dice);
            Page = duppo::Pixels::PageFor(Duppo.Body.Scale, On.Unit);
            Make();
        }
    }
}

/* Duppo in cells, on a text page. */
int FloatInCells()
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
    Overlay.SetMosaics(Screen.Mosaics());

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
    Picture.Dress(duppo::Palette::Make(Set.Body, Set.Ring, Set.Pupil), Set.Smooth);
    Body.Begin(Glass, SizeOf(Set, Glass), Dice);
    Eye.Begin(Glass);
    Moons.Begin(Body.Center, Body.Scale);
    box::settings::watch Watch;

    // Painted until it can be no more — the display gone, or Duppo
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    box::pacer Pace;
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            Body.Tune(Set.Speed);
            Eye.Tune(Set.Speed, Set.Blink);
            Turning.Tune(Set.Speed);
            Moons.Tune(Set.Speed, Set.Satellites);
            Picture.Dress(duppo::Palette::Make(Set.Body, Set.Ring, Set.Pupil), Set.Smooth);
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
        std::this_thread::sleep_for(Pace.wait(box::pacer::interval(Set.Fps)));
    }
    (void)Screen.Close();
    return 1;
}

}  // namespace

int main()
{
    /* An area over everything, shown always, as large as the screen: on a
     * framebuffer Duppo floats in its pixels; on a text page in cells. */
    {
        constexpr float Largest = static_cast<float>(ITEM_REACH - 1) / ITEM_POINT;
        std::expected<painter::gui::Area, box::error> Opened =
            painter::gui::Area::Open(painter::gui::Placement::Over, painter::core::PointF{Largest, Largest}, true);
        if (Opened && Opened->On() == painter::gui::Glass::Pixels) return FloatOnPixels(std::move(*Opened));
    }
    return FloatInCells();
}
