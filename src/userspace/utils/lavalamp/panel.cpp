/* panel.cpp — the tuning panel as items. See panel.h. Laid in points: the
 * panel 340 wide at the left of the screen, a row 34 tall. */

#include "panel.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace lavalamp {

namespace {

using namespace std::chrono_literals;
using painter::core::Corners;

constexpr float Margin = 40, Wide = 340, RowTall = 34, RowsTop = 58;
constexpr float Tall   = RowsTop + Rows * RowTall + 40;
constexpr float Middle = 150, BarLong = 110, ValueAt = 180, ValueWide = 138;
constexpr float SwitchLong = 36, SwitchTall = 18, Knob = 14;

const Color Faint = Color::Rgba(255, 255, 255, 48);

float RowY(int At)
{
    return RowsTop + static_cast<float>(At) * RowTall;
}

gui::TextBox ToRight()
{
    gui::TextBox Box;
    Box.Align = ITEM_TEXT_RIGHT;
    Box.Wrap  = false;
    return Box;
}

gui::TextBox Centred()
{
    gui::TextBox Box;
    Box.Align = ITEM_TEXT_CENTER;
    Box.Wrap  = false;
    return Box;
}

gui::Fill Strip(const Composition &Lamp, float Long)
{
    gui::Fill Made;
    Made.What  = gui::Fill::Kind::Linear;
    Made.To    = {Long, 0};
    Made.Stops = {gui::Stop{0, Lamp.Wax}, gui::Stop{0.5f, Lamp.Rim}, gui::Stop{1, Lamp.Glass}};
    return Made;
}

}  // namespace

Panel::Panel(gui::Area &On) : Screen(On)
{
    Whole = Screen.Group();
    Plate = Whole.Rectangle(gui::Role::Panel);
    Plate.SetSize({Wide, Tall}).SetCorners(Corners::All(18)).SetOpacity(0.88f);
    Plate.SetShadow(gui::Shadow{{0, 12}, 36, 0, Color::Rgba(0, 0, 0, 120), false});
    Chosen = Whole.Rectangle(gui::Role::Selection);
    Chosen.SetPlace({8, RowY(0) + 2}).SetSize({Wide - 16, RowTall - 4}).SetCorners(Corners::All(9));
    Title = Whole.Text("lavalamp", gui::Role::Heading);
    Title.SetPlace({22, 14});
    for (int At = 0; At < Rows; At++) {
        Line &One  = Lines[static_cast<std::size_t>(At)];
        const Row Of = static_cast<Row>(At);
        One.Name   = Whole.Text(Tuning::Name(Of), gui::Role::Label);
        One.Name.SetPlace({22, RowY(At) + 8});
        One.Swatch = Whole.Rectangle();
        One.Track  = Whole.Rectangle();
        One.Bar    = Whole.Rectangle();
        One.Value  = Whole.Text("", gui::Role::Body);
        One.Value.SetPlace({ValueAt, RowY(At) + 7}).SetSize({ValueWide, gui::ByContent}).SetBox(ToRight());
        gui::Stroke Edge;
        Edge.Width = 1;
        Edge.With  = gui::Fill::Of(Color::Rgba(255, 255, 255, 90));
        if (Of == Row::Palette) One.Swatch.SetPlace({Middle, RowY(At) + 10}).SetSize({64, 14}).SetCorners(Corners::All(7)).SetStroke(Edge);
        else if (Tuning::IsColor(Of)) One.Swatch.SetPlace({Middle, RowY(At) + 6}).SetSize({22, 22}).SetCorners(Corners::All(6)).SetStroke(Edge);
        else One.Swatch.Hide();
        if (Of == Row::Palette || Tuning::IsColor(Of)) {
            One.Track.Hide();
            One.Bar.Hide();
        } else if (Tuning::IsSwitch(Of)) {
            /* A switch: a pill the theme's accent fills when it is on, its
             * knob gliding to that end. */
            One.Track.SetPlace({Middle, RowY(At) + (RowTall - SwitchTall) / 2}).SetSize({SwitchLong, SwitchTall}).SetCorners(Corners::All(SwitchTall / 2));
            One.Bar.SetSize({Knob, Knob}).SetCorners(Corners::All(Knob / 2)).SetFill(gui::Fill::Of(Color::Rgb(0xFF, 0xFF, 0xFF)));
        } else {
            One.Track.SetPlace({Middle, RowY(At) + 14}).SetSize({BarLong, 6}).SetCorners(Corners::All(3)).SetFill(gui::Fill::Of(Faint));
            One.Bar.SetPlace({Middle, RowY(At) + 14}).SetCorners(Corners::All(3)).SetFill(gui::Fill::OfTheme(gui::ThemeColor::Accent));
        }
    }
    Note = Whole.Text("", gui::Role::Hint);
    Note.SetPlace({22, RowY(Rows) + 10});
    Whole.Hide();

    Keys = Screen.Text("", gui::Role::Hint);
    Keys.SetBox(Centred()).SetShadow(gui::Shadow{{0, 1}, 6, 0, Color::Rgba(0, 0, 0, 170), false});
    Place();
}

void Panel::Place()
{
    const painter::core::PointF Size = Screen.Size();
    Whole.SetPlace({Margin, std::max(16.0f, (Size.Y - Tall) / 2)});
    Keys.SetPlace({0, Size.Y - 44}).SetSize({Size.X, gui::ByContent});
}

void Panel::ShowLine(int At, const Tuning &State, const Look &Now)
{
    Line     &One = Lines[static_cast<std::size_t>(At)];
    const Row Of  = static_cast<Row>(At);
    if (State.Typing() && Of == State.Chosen()) {
        gui::TextStyle Typed;
        Typed.Color = Color::Rgb(0xFF, 0xFF, 0xFF);
        One.Value.SetText(std::string(State.Typed()) + "|").SetStyle(Typed);
    } else One.Value.SetText(Tuning::Value(Of, Now)).SetStyle({});
    if (Of == Row::Palette) One.Swatch.SetFill(Strip(Now.Lamp, 64));
    else if (Tuning::IsColor(Of)) One.Swatch.SetFill(gui::Fill::Of(Tuning::ColorOf(Of, Now)));
    else if (Tuning::IsSwitch(Of)) {
        const bool  On   = Tuning::On(Of, Now);
        const float Edge = (SwitchTall - Knob) / 2;
        One.Track.SetFill(On ? gui::Fill::OfTheme(gui::ThemeColor::Accent) : gui::Fill::Of(Faint));
        const painter::core::PointF Place{Middle + (On ? SwitchLong - Knob - Edge : Edge), RowY(At) + (RowTall - Knob) / 2};
        if (!Started) One.Bar.SetPlace(Place);
        else if (On != One.On) Screen.Animate(gui::Animation::Spring(240ms, 0.1f), [&] { One.Bar.SetPlace(Place); });
        One.On = On;
    } else One.Bar.SetSize({std::max(6.0f, BarLong * Tuning::Share(Of, Now)), 6});
}

void Panel::Show(const Tuning &State, const Look &Now)
{
    for (int At = 0; At < Rows; At++) ShowLine(At, State, Now);
    Note.SetText(State.Note());
    if (State.Chosen() != Marked) {
        Marked = State.Chosen();
        Screen.Animate(gui::Animation::Spring(280ms, 0.12f), [&] { Chosen.SetPlace({8, RowY(static_cast<int>(Marked)) + 2}); });
    }

    /* The line of keys stays while the panel is open; shown at the start and
     * when it closes, it fades a few seconds after. */
    Keys.SetText(State.Hint());
    const bool Changed = State.Open() != Opened || !Started;
    if (!Changed) return;
    Started = true;
    Opened  = State.Open();
    if (Opened) {
        Whole.Show().SetOpacity(0);
        const painter::core::PointF Size = Screen.Size();
        Whole.SetPlace({Margin - 24, std::max(16.0f, (Size.Y - Tall) / 2)});
        Screen.Animate(gui::Animation::Spring(380ms, 0.1f), [&] {
            Whole.SetOpacity(1);
            Place();
        });
        Keys.SetOpacity(1);
        return;
    }
    Screen.Animate(gui::Animation::Out(220ms), [&] { Whole.SetOpacity(0); });
    Keys.SetOpacity(1);
    gui::Animation Fade = gui::Animation::Out(900ms);
    Fade.Delay          = 3500ms;
    Screen.Animate(Fade, [&] { Keys.SetOpacity(0); });
}

}  // namespace lavalamp
