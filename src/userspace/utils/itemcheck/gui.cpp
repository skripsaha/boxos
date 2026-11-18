/* itemcheck for the programmer — painter::gui as a program writes with it,
 * held on the machine: objects told what changed and what did not, a frame of
 * a list described whole — one row changed, all reordered, drawn again as it
 * was — a canvas of the program's own pixels drawn page after page, things
 * in time played by the display and their ends told, a text measured
 * beforehand against the size the display laid it at, and the C face
 * (from_c.c). See itemcheck_gui.h. */

#include "itemcheck_gui.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "box/clock.h"
#include "itemcheck_c.h"
#include "painter/gui/area.h"
#include "painter/gui/frame.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"

namespace {

using namespace std::chrono_literals;
namespace gui = painter::gui;
using painter::core::PointF;

struct Heard {
    std::uint32_t           Refused = 0;
    std::vector<gui::Event> Sizes;   // items sized by their content, as told
};

/* What the display tells, waited for until the frame showing Step: that
 * frame's Shown; every refusal counted, the first said. */
gui::Event ShownBy(gui::Area &On, std::uint64_t Step, Heard &Of)
{
    for (;;) {
        const gui::Event Each = On.Wait();
        if (Each.What == gui::Event::Kind::Refused && Of.Refused++ == 0)
            std::printf("itemcheck: the display refused item %u's property %u — %d\n", Each.Item, Each.Property, static_cast<int>(Each.Reason));
        if (Each.What == gui::Event::Kind::Size) Of.Sizes.push_back(Each);
        if (Each.What == gui::Event::Kind::Shown && Each.Step >= Step) return Each;
    }
}

}  // namespace

bool CheckForProgrammer()
{
    std::expected<gui::Area, box::error> Opened = gui::Area::Open(gui::Placement::Over, PointF{360, 260});
    if (!Opened) {
        std::printf("itemcheck: FAIL — the display opened no area for the programmer's checks\n");
        return false;
    }
    gui::Area &On     = *Opened;
    const bool Pixels = On.On() == gui::Glass::Pixels;
    Heard      Of;
    bool       Holds = true;

    /* Objects: the time told each step with its place, and a panel's size
     * told again — only the time goes. */
    gui::Text Time = On.Text("12:00", gui::Role::Heading);
    Time.SetPlace({16, 12});
    gui::Shape Plate = On.Rectangle(gui::Role::Panel);
    Plate.SetPlace({16, 48}).SetSize({200, 40});
    (void)On.Present();
    (void)ShownBy(On, On.Step(), Of);
    std::uint64_t Bytes = 0;
    for (int I = 0; I < 30; I++) {
        char Said[8];
        std::snprintf(Said, sizeof Said, "12:%02d", I + 1);
        Time.SetText(Said).SetPlace({16, 12});
        Plate.SetSize({200, 40});
        (void)On.Present();
        Bytes += ShownBy(On, On.Step(), Of).Bytes;
    }
    std::printf("itemcheck: objects — a text told its time 30 times with its place, a panel's size told again — %llu bytes of the "
                "ring a step\n",
                static_cast<unsigned long long>(Bytes / 30));
    Holds = Holds && Bytes / 30 <= 64;

    /* A frame: a list of 24 rows in a group, each keyed by its row. */
    std::vector<int>         Order(24);
    std::vector<std::string> Names(24);
    std::iota(Order.begin(), Order.end(), 0);
    for (int Row = 0; Row < 24; Row++) Names[static_cast<std::size_t>(Row)] = "row " + std::to_string(Row + 1);
    const auto List = [&] {
        On.Draw([&](gui::Frame &Into) {
            gui::Group Rows = Into.Group();
            Rows.SetPlace({240, 96});
            for (int At = 0; At < 24; At++) {
                const int Row = Order[static_cast<std::size_t>(At)];
                Rows.Text(Names[static_cast<std::size_t>(Row)], gui::Role::Hint).Key(static_cast<std::uint64_t>(Row)).SetPlace({0, 6.0f * At});
            }
        });
    };
    List();
    (void)ShownBy(On, On.Step(), Of);
    Names[5] = "row 6, changed";
    List();
    const gui::Event One = ShownBy(On, On.Step(), Of);
    std::reverse(Order.begin(), Order.end());
    List();
    const gui::Event Reordered = ShownBy(On, On.Step(), Of);
    const std::uint64_t Before = On.Step();
    List();
    const bool Quiet = On.Step() == Before;
    std::printf("itemcheck: a frame of 24 rows in a group — one row changed: %u records; all reordered: %u records; drawn again "
                "as it was: %s\n",
                One.Records, Reordered.Records, Quiet ? "no step" : "a step");
    Holds = Holds && One.Records <= 3 && Quiet;

    /* A canvas of the program's own pixels, drawn page after page. */
    if (Pixels) {
        gui::Canvas Board = On.Canvas({96, 64});
        Board.SetPlace({240, 12});
        int Drawn = 0;
        for (int I = 0; I < 6; I++) {
            const auto Paint = [&](painter::core::Canvas &Page) {
                Page.Clear(painter::core::Color::Rgb(static_cast<std::uint8_t>(40 * I), 120, static_cast<std::uint8_t>(200 - 30 * I)));
            };
            if (Board.Draw(Paint)) Drawn++;
            (void)On.Present();
            (void)ShownBy(On, On.Step(), Of);
        }
        std::printf("itemcheck: a canvas of 96x64 pixels drawn %d times, its two pages taken in turn\n", Drawn);
        Holds = Holds && Drawn == 6;
    }

    /* Things in time: a plate faded in, played by the display while this
     * waits, its end told by its time; two one after another, the second
     * ending the moment its own time after the first; one cut, told cut. On
     * a text page every change is at once, its end told at once. */
    {
        gui::Shape Plate2 = On.Rectangle(gui::Role::Panel);
        Plate2.SetPlace({16, 100}).SetSize({120, 24}).SetOpacity(0);
        (void)On.Present();
        (void)ShownBy(On, On.Step(), Of);
        const auto Numbered = [](gui::Animation How, std::uint32_t Number, std::uint32_t After = 0) {
            How.Number = Number;
            How.After  = After;
            return How;
        };
        const auto Ending = [&](std::uint32_t Number) -> std::optional<gui::Event> {
            for (;;) {
                const gui::Event Each = On.Wait();
                if (Each.What == gui::Event::Kind::Refused && Of.Refused++ == 0)
                    std::printf("itemcheck: the display refused item %u's property %u — %d\n", Each.Item, Each.Property, static_cast<int>(Each.Reason));
                if (Each.What == gui::Event::Kind::AnimationEnd && Each.Number == Number) return Each;
            }
        };
        std::uint64_t Began = clock_uptime_us();
        On.Animate(Numbered(gui::Animation::Linear(300ms), 71), [&] { Plate2.SetOpacity(1); });
        (void)On.Present();
        const std::optional<gui::Event> Faded = Ending(71);
        const std::uint64_t             Fade  = (clock_uptime_us() - Began) / 1000;
        Began = clock_uptime_us();
        On.Animate(Numbered(gui::Animation::Out(200ms), 72), [&] { Plate2.SetPlace({200, 100}); });
        On.Animate(Numbered(gui::Animation::Out(200ms), 73, 72), [&] { Plate2.SetPlace({200, 160}); });
        (void)On.Present();
        const std::optional<gui::Event> First = Ending(72);
        const std::optional<gui::Event> Then  = Ending(73);
        const std::uint64_t             Chain = (clock_uptime_us() - Began) / 1000;
        On.Animate(Numbered(gui::Animation::Linear(1000ms), 74), [&] { Plate2.SetPlace({16, 100}); });
        (void)On.Present();
        Plate2.SetPlace({40, 100});
        (void)On.Present();
        const std::optional<gui::Event> Cut = Ending(74);
        const std::uint64_t Most = Pixels ? 150 : 100;
        const bool Timed = Pixels ? Fade >= 300 && Fade <= 300 + Most && Chain >= 400 && Chain <= 400 + Most : Fade <= Most && Chain <= Most;
        const bool Told  = Faded && Faded->Finished && First && First->Finished && Then && Then->Finished && Cut && !Cut->Finished;
        std::printf("itemcheck: in time — a plate faded in 300 ms, its end told after %llu ms; moved twice one after another, "
                    "both ends told after %llu ms; a move said over, told cut: %s\n",
                    static_cast<unsigned long long>(Fade), static_cast<unsigned long long>(Chain), Told ? "every end as it was" : "NOT every end as it was");
        Holds = Holds && Timed && Told;
    }

    /* A text measured beforehand, then laid by the display sized by its
     * content: the size it tells. */
    const PointF Measured = gui::Measure(On, "Measured beforehand", gui::Role::Body);
    gui::Text    Sized    = On.Text("Measured beforehand", gui::Role::Body);
    Sized.SetPlace({16, 220});
    (void)On.Present();
    (void)ShownBy(On, On.Step(), Of);
    std::optional<PointF> Laid;
    for (const gui::Event &Each : Of.Sizes)
        if (Each.Item == Sized.Number()) Laid = Each.Size;
    if (Pixels) {
        std::printf("itemcheck: a text measured beforehand %.2fx%.2f points, the display laid it at %.2fx%.2f\n", Measured.X, Measured.Y,
                    Laid ? Laid->X : -1.0f, Laid ? Laid->Y : -1.0f);
        Holds = Holds && Laid && Laid->X == Measured.X && Laid->Y == Measured.Y;
    } else {
        std::printf("itemcheck: a text measured beforehand %.0fx%.0f points, a cell a character on the text page\n", Measured.X, Measured.Y);
    }

    /* The C face. */
    float      Wide = 0, Tall = 0;
    const bool FromC = ItemcheckFromC(&Wide, &Tall) != 0;
    std::printf("itemcheck: from C — a group, a heading and a rectangle made and set, the heading measured %.1fx%.1f points, "
                "changed and removed: %s\n",
                Wide, Tall, FromC ? "every step shown" : "NOT every step shown");
    Holds = Holds && FromC && Wide > 0 && Tall > 0 && Of.Refused == 0;
    return Holds;
}
