/* itemcheck keys — the keyboard's way to the things a program shows, held on
 * the machine with keys typed from outside, as the gate types them: a field,
 * two buttons side by side and a third not to be used. Once it says it is
 * ready, the keys Tab h i Tab → Enter, the labels' chord and s are typed:
 * Tab reaches the field and the display types into it, telling its text;
 * Tab and → reach the second button and Enter presses it; the chord's labels
 * and the letter of the first reach and press it; the focus the program asks
 * back to the field comes back; the one not to be used is never reached. See
 * itemcheck_keys.h. */

#include "itemcheck_keys.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "box/alarm.h"
#include "box/clock.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

namespace {

namespace gui = painter::gui;
using painter::core::PointF;

/* How long the keys may take to come, at most. */
constexpr std::uint64_t WaitMs = 90000;

}  // namespace

bool CheckKeys()
{
    std::expected<gui::Area, box::error> Opened = gui::Area::Open(gui::Placement::Over, PointF{420, 160});
    if (!Opened) {
        std::printf("itemcheck keys: FAIL — the display opened no area\n");
        return false;
    }
    gui::Area &On    = *Opened;
    gui::Text  Field = On.Text("", gui::Role::Field);
    Field.SetPlace({20, 20}).SetSize({260, gui::ByContent});
    gui::Text One = On.Text("One", gui::Role::Button);
    One.SetPlace({20, 90});
    gui::Text Two = On.Text("Two", gui::Role::Button);
    Two.SetPlace({150, 90});
    gui::Text Three = On.Text("Three", gui::Role::Button);
    Three.SetPlace({280, 90}).SetDisabled(true);
    (void)On.Present();
    /* Laid on the glass before the keys come, so the display knows where. */
    for (;;) {
        const gui::Event Each = On.Wait();
        if (Each.What == gui::Event::Kind::Shown && Each.Step >= On.Step()) break;
    }
    std::printf("itemcheck keys: ready\n");

    std::vector<std::uint32_t> Came, Pressed;
    std::string                Typed;
    bool                       Asked = false, Late = false;
    const std::uint64_t        Until = clock_uptime_ms() + WaitMs;
    (void)alarm_set(Until);
    while (!Asked && !Late) {
        const gui::Event Each = On.Wait();
        switch (Each.What) {
        case gui::Event::Kind::Focus:
            if (Each.Came) Came.push_back(Each.Item);
            /* Both pressed: the focus asked back to the field. */
            if (Each.Came && Pressed.size() == 2 && Each.Item == Field.Number()) Asked = true;
            break;
        case gui::Event::Kind::Text:
            if (Each.Item == Field.Number()) Typed = Each.Text;
            break;
        case gui::Event::Kind::Press:
            Pressed.push_back(Each.Item);
            if (Pressed.size() == 2) {
                Field.Focus();
                (void)On.Present();
            }
            break;
        case gui::Event::Kind::Alarm:
            Late = clock_uptime_ms() >= Until;
            break;
        default:
            break;
        }
    }
    const bool Never = std::find(Came.begin(), Came.end(), Three.Number()) == Came.end();
    const bool Order = Pressed.size() == 2 && Pressed[0] == Two.Number() && Pressed[1] == One.Number();
    const bool Holds = !Late && Typed == "hi" && Order && Never && Asked;
    std::printf("itemcheck keys: the field typed \"%s\", pressed %s, the focus come %zu times, %s, asked back: %s\n", Typed.c_str(),
                Order ? "two by Enter then one by its label" : "NOT two then one", Came.size(),
                Never ? "never to the one not to be used" : "TO THE ONE NOT TO BE USED", Asked ? "came" : "never came");
    std::printf("itemcheck keys: %s\n", Holds ? "PASS — reached, typed into and pressed by the keyboard alone"
                                              : "FAIL — the keyboard's way to the things did not hold");
    return Holds;
}
