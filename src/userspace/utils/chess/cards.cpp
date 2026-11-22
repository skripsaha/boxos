/* cards.cpp — the menu and the verdict, as items. See cards.h. */

#include "cards.h"

#include <chrono>
#include <string>

#include "engine.h"
#include "look.h"

namespace chess {

namespace {

using namespace std::chrono_literals;
using painter::core::Color;
using painter::core::Corners;
using painter::core::PointF;

constexpr float MenuWide = 440, MenuTall = 340, RowsTop = 112, RowTall = 50;
constexpr float VerdictWide = 380, VerdictTall = 168;

const Color Plain = Color::Rgb(0xF2, 0xED, 0xE4);

gui::TextStyle Style(float Size, std::uint16_t Weight, Color With = Plain)
{
    gui::TextStyle Made;
    Made.Size   = Size;
    Made.Weight = Weight;
    Made.Color  = With;
    return Made;
}

gui::TextBox Aligned(std::uint8_t Align)
{
    gui::TextBox Made;
    Made.Align = Align;
    Made.Wrap  = false;
    return Made;
}

float RowY(Ask Row)
{
    return RowsTop + static_cast<float>(Row) * RowTall;
}

}  // namespace

Menu::Menu(gui::Area &On) : Screen(On)
{
    const PointF Size = Screen.Size();
    Whole             = Screen.Group();
    Dim               = Whole.Rectangle();
    Dim.SetSize(Size).SetFill(gui::Fill::Of(Color::Rgba(0, 0, 0, 0x90)));
    const PointF Corner{(Size.X - MenuWide) / 2, (Size.Y - MenuTall) / 2};
    Card = Whole.Rectangle(gui::Role::Panel);
    Card.SetPlace(Corner).SetSize({MenuWide, MenuTall}).SetCorners(Corners::All(20));
    Card.SetShadow(gui::Shadow{{0, 18}, 50, 0, Color::Rgba(0, 0, 0, 0xA0), false});
    Marker = Whole.Rectangle(gui::Role::Selection);
    Marker.SetSize({MenuWide - 24, RowTall - 8}).SetCorners(Corners::All(12)).SetPlace({Corner.X + 12, Corner.Y + RowY(Ask::Opponent) + 4});

    Title = Whole.Text("Chess", gui::Role::Heading);
    Title.SetPlace({Corner.X + 30, Corner.Y + 30}).SetStyle(Style(38, 750));
    Ornaments[0] = std::make_unique<Figure>(Whole, PieceOf(White, King), 72);
    Ornaments[1] = std::make_unique<Figure>(Whole, PieceOf(Black, Queen), 72);
    Ornaments[0]->Whole().SetPlace({Corner.X + MenuWide - 150, Corner.Y + 16});
    Ornaments[1]->Whole().SetPlace({Corner.X + MenuWide - 96, Corner.Y + 20});

    constexpr const char *Names[3] = {"Opponent", "You play", "Level"};
    for (std::size_t At = 0; At < Lines.size(); At++) {
        const float Y = Corner.Y + RowY(static_cast<Ask>(At)) + 14;
        Lines[At].Name = Whole.Text(Names[At], gui::Role::Label);
        Lines[At].Name.SetPlace({Corner.X + 30, Y}).SetStyle(Style(17, 600));
        Lines[At].Value = Whole.Text("", gui::Role::Body);
        Lines[At].Value.SetPlace({Corner.X + 200, Y}).SetSize({MenuWide - 230, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_RIGHT)).SetStyle(Style(17, 600));
    }
    for (std::size_t At = 0; At < Pips.size(); At++) {
        Pips[At] = Whole.Rectangle();
        Pips[At]
            .SetPlace({Corner.X + 200 + static_cast<float>(At) * 18, Corner.Y + RowY(Ask::Level) + 21})
            .SetSize({12, 12})
            .SetCorners(Corners::All(6));
    }
    Hint = Whole.Text("", gui::Role::Hint);
    Hint.SetPlace({Corner.X, Corner.Y + MenuTall - 44}).SetSize({MenuWide, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_CENTER)).SetStyle(Style(13, 500));
    Whole.Hide();
}

void Menu::Show(const Choice &Now, Ask Row, bool Resumable)
{
    const PointF Corner{(Screen.Size().X - MenuWide) / 2, (Screen.Size().Y - MenuTall) / 2};
    Lines[0].Value.SetText(Now.Computer ? "‹  the computer  ›" : "‹  a friend  ›");
    constexpr const char *Sides[3] = {"‹  white  ›", "‹  black  ›", "‹  either  ›"};
    Lines[1].Value.SetText(Sides[static_cast<int>(Now.Side)]);
    Lines[2].Value.SetText(std::string("‹  ") + std::to_string(Now.Level) + "  ›");
    for (std::size_t At = 1; At < Lines.size(); At++) {
        Lines[At].Name.SetOpacity(Now.Computer ? 1.0f : 0.35f);
        Lines[At].Value.SetOpacity(Now.Computer ? 1.0f : 0.35f);
    }
    for (std::size_t At = 0; At < Pips.size(); At++)
        Pips[At]
            .SetFill(static_cast<int>(At) < Now.Level ? gui::Fill::OfTheme(gui::ThemeColor::Accent) : gui::Fill::Of(Color::Rgba(255, 255, 255, 0x30)))
            .SetOpacity(Now.Computer ? 1.0f : 0.35f);
    Hint.SetText(std::string("↑ ↓ choose    ← → change    enter plays    esc ") + (Resumable ? "goes back" : "leaves"));
    if (Row != Marked) {
        Marked = Row;
        const PointF Goal{Corner.X + 12, Corner.Y + RowY(Row) + 4};
        Screen.Animate(gui::Animation::Spring(260ms, 0.1f), [&] { Marker.SetPlace(Goal); });
    }
    if (Shown) return;
    Shown = true;
    Whole.Show().SetOpacity(0);
    Screen.Animate(gui::Animation::Out(220ms), [&] { Whole.SetOpacity(1); });
}

void Menu::Hide()
{
    if (!Shown) return;
    Shown = false;
    Whole.Hide();
}

Verdict::Verdict(gui::Area &On, PointF At) : Screen(On), Middle(At)
{
    Whole = Screen.Group();
    Whole.SetAnchor({VerdictWide / 2, VerdictTall / 2}).SetPlace({Middle.X - VerdictWide / 2, Middle.Y - VerdictTall / 2});
    Card = Whole.Rectangle(gui::Role::Panel);
    Card.SetSize({VerdictWide, VerdictTall}).SetCorners(Corners::All(20)).SetOpacity(0.96f);
    Card.SetShadow(gui::Shadow{{0, 16}, 44, 0, Color::Rgba(0, 0, 0, 0xA0), false});
    Title = Whole.Text("", gui::Role::Heading);
    Title.SetPlace({0, 26}).SetSize({VerdictWide, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_CENTER)).SetStyle(Style(32, 750));
    Line = Whole.Text("", gui::Role::Body);
    Line.SetPlace({0, 76}).SetSize({VerdictWide, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_CENTER)).SetStyle(Style(18, 600));
    Hint = Whole.Text("n new game    u takes back    esc leaves", gui::Role::Hint);
    Hint.SetPlace({0, VerdictTall - 40}).SetSize({VerdictWide, gui::ByContent}).SetBox(Aligned(ITEM_TEXT_CENTER)).SetStyle(Style(13, 500));
    Whole.Hide();
}

void Verdict::Show(std::string_view Said, std::string_view Why)
{
    Title.SetText(Said);
    Line.SetText(Why);
    Whole.Show().SetOpacity(0).SetScale({0.85f, 0.85f});
    gui::Animation In = gui::Animation::Spring(420ms, 0.14f);
    In.Delay          = 300ms;
    Screen.Animate(In, [&] { Whole.SetOpacity(1).SetScale({1, 1}); });
}

void Verdict::Hide()
{
    Whole.Hide();
}

}  // namespace chess
