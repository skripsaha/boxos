/* paint.cpp — the tower as a picture: the room, the wizard and the cauldron in
 * pixels, the shelf of things, and the words round them in cells of text. */

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "cauldron.h"
#include "icons.h"
#include "tower.h"

namespace wizard {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr int Pitch = IconSize + 1;

// How many things the shelf shows, which is the first, and where the row begins.
struct ShelfView {
    int First, Visible, Margin;
};

ShelfView ViewOf(const Scene &Where, std::size_t Pick)
{
    const int Count = static_cast<int>(IngredientCount);
    const int Visible = std::min(Count, std::max(1, (Where.Cols - 4) / Pitch));
    const int First = std::clamp(static_cast<int>(Pick) - Visible / 2, 0, Count - Visible);
    return ShelfView{First, Visible, (Where.Cols - Visible * Pitch + 1) / 2};
}

// A key and what it does, as the bottom line has them; Rank is how soon it is dropped when the line is too long.
struct Hint {
    std::string Key;
    std::string Does;
    int         Rank;
    bool        Urgent;
};

// The text page has sixteen colors and tells a color by its dominant hue: a dim
// brown stone would become dark red. So what is dim and only a little colored
// is made grey, and the page keeps its reds, blues and yellows for what is
// meant to be colored: wood, cloth, fire, the wizard's hat.
void ForTextPage(Canvas &Out)
{
    for (int Y = 0; Y < Out.Height; Y++) {
        for (int X = 0; X < Out.Width; X++) {
            const Color Here = Out.At(X, Y);
            const int High = std::max({Here.Red(), Here.Green(), Here.Blue()}), Low = std::min({Here.Red(), Here.Green(), Here.Blue()});
            const int Lightness = (High + Low) / 2;
            if (High == 0 || Lightness >= 150) continue;
            const int Saturation = (High - Low) * 255 / High;
            if (Saturation < 132 || (Saturation < 200 && Lightness < 85)) {
                const auto Grey = static_cast<std::uint8_t>(Lightness);
                Out.At(X, Y) = Color::Rgb(Grey, Grey, Grey);
            } else if (Saturation < 200 && Here.Red() == High && Lightness < 128) {
                // Dim and orange-red is wood, not blood: turned a little towards yellow it is the page's brown.
                const int Green = Low + (High - Low) * 8 / 10;
                Out.At(X, Y) = Color::Rgb(Here.Red(), static_cast<std::uint8_t>(Green), Here.Blue());
            }
        }
    }
}

}  // namespace

void Tower::DrawShelf()
{
    const ShelfView View = ViewOf(Where, Pick);
    const Color Wood = Color::Rgb(0x3A, 0x26, 0x16);
    Picture.Fill(0, Where.ShelfTop, Where.Width, IconSize, Wood);
    for (int X = 0; X < Where.Width; X++) {
        for (int Y = 0; Y < IconSize; Y++) Picture.Put(X, Where.ShelfTop + Y, Toned(Wood, 0.80f + 0.35f * Grain(X / 2, Y, 301) - 0.06f * static_cast<float>(Y)));
        Picture.Put(X, Where.ShelfTop + IconSize - 1, Toned(Wood, 0.55f));
    }
    for (int Place = 0; Place < View.Visible; Place++) {
        const std::size_t Which = static_cast<std::size_t>(View.First + Place);
        const int X = View.Margin + Place * Pitch;
        const bool Here = Which == Pick;
        int Rise = 0;
        if (Here) {
            Rise = Beat(LastMs, 1100) > 0.55f ? -1 : 0;
            Picture.Glow(static_cast<float>(X + IconSize / 2), static_cast<float>(Where.ShelfTop + IconSize / 2), 9.0f, Chosen.Glow, 120);
        }
        DrawIcon(Picture, X, Where.ShelfTop + Rise, Which, Here ? 340 : 270, Here ? 70 : 0);
    }
}

void Tower::Paint()
{
    const Show Now = ShowNow();
    Hall.Draw(Picture, Now);
    DrawCauldron(Picture, Where, Now, Hall.Light());
    Wizard.Draw(Picture, Where, Posed(), Now, Hall.Light());
    if (Falling) {
        const Spot Surface = SurfaceOf(Where);
        const float T = std::min(1.0f, static_cast<float>(LastMs - Falling->Began) / static_cast<float>(FlightMs));
        const float X = Falling->FromX + (Surface.X - Falling->FromX) * T;
        const float Y = Falling->FromY + (Surface.Y - Falling->FromY) * T - 10.0f * Where.Unit * Wave(T * 0.5f);
        DrawIcon(Picture, static_cast<int>(X) - IconSize / 2, static_cast<int>(Y) - IconSize / 2, Falling->Which, 380, 50);
    }
    Dust.Draw(Picture);
    DrawShelf();

    if (Screen.Glass() == tui::GlassKind::Text) ForTextPage(Picture);
    tui::Frame &Out = Screen.FrameBuffer();
    Out.PutPixels(Picture.All());
    PaintBar(Out);
    PaintShelf(Out);
    PaintHints(Out);
    Said.Paint(Out, Where, Chosen, LastMs);
    if (Reading.Open) PaintBook(Out);
    if (Help) PaintHelp(Out);
}

void Tower::PaintBar(tui::Frame &Out) const
{
    const core::Size Size = Screen.Size();
    Out.Fill(core::Rect{0, 0, Size.Width, 1}, ' ', {Chosen.Text, Chosen.Bars});
    tui::PutText(Out, core::Point{1, 0}, "wizard", Size.Width - 2, Chosen.Title, Chosen.Bars);
    const std::int32_t Room = std::max(0, Size.Width - 10 - 38);
    if (LaidUntilMs != 0 && Spoken)
        tui::PutText(Out, core::Point{9, 0}, std::format("{}  {} s", Spoken->Name, LeftSeconds()), Room, Chosen.Glow, Chosen.Bars);
    else
        tui::PutText(Out, core::Point{9, 0}, std::format("grimoire: {} of {} potions found", Book.KnownCount(), RecipeCount), Room, Chosen.Quiet, Chosen.Bars);
}

void Tower::PaintShelf(tui::Frame &Out) const
{
    namespace G = tui::Glyphs;
    const core::Size Size = Screen.Size();
    const ShelfView View = ViewOf(Where, Pick);
    const std::int32_t Top = Size.Height - ShelfRows;
    for (int Place = 0; Place < View.Visible; Place++) {
        const std::size_t Which = static_cast<std::size_t>(View.First + Place);
        const std::int32_t X = View.Margin + Place * Pitch;
        if (Which == Pick) {
            for (std::int32_t Row = Top; Row < Top + IconSize / 2; Row++) {
                Out.Put({X - 1, Row}, G::RightHalf, {Chosen.Glow, Out.Read({X - 1, Row}).Background});
                Out.Put({X + IconSize, Row}, G::LeftHalf, {Chosen.Glow, Out.Read({X + IconSize, Row}).Background});
            }
        }
        const auto Count = static_cast<int>(std::count(Brewing.begin(), Brewing.end(), static_cast<std::uint8_t>(Which)));
        if (Count > 0) Out.Put({X + IconSize - 1, Top}, static_cast<char>('0' + Count), {Chosen.Title, Color::Rgb(0x90, 0x30, 0x20)});
    }
    if (View.First > 0) Out.Put({0, Top + 1}, G::Left, {Chosen.Glow, Chosen.Bars});
    if (View.First + View.Visible < static_cast<int>(IngredientCount)) Out.Put({Size.Width - 1, Top + 1}, G::Right, {Chosen.Glow, Chosen.Bars});

    // Under them: what he is looking at, and what is in the pot.
    const std::int32_t Row = Size.Height - 2;
    const Color Panel = Mixed(Chosen.Bars, Color::Rgb(0x60, 0x44, 0x28), 60);
    Out.Fill(core::Rect{0, Row, Size.Width, 1}, ' ', {Chosen.Text, Panel});
    const Ingredient &Thing = Ingredients()[Pick];
    std::string Inside;
    for (std::uint8_t One : Brewing) Inside += (Inside.empty() ? "" : " + ") + std::string(Ingredients()[One].Name);
    const std::string Pot = Brewing.empty() ? std::string("the pot is empty") : std::format("in the pot: {}", Inside);
    const auto PotWide = static_cast<std::int32_t>(tui::CellsOf(Pot));
    const std::int32_t Left = Size.Width - PotWide - 1;
    std::int32_t At = 1 + tui::PutText(Out, core::Point{1, Row}, Thing.Name, Size.Width - 2, Chosen.Title, Panel);
    At += tui::PutText(Out, core::Point{At + 1, Row}, std::format("- {}", Thing.Spirit), std::max(0, Left - At - 2), Chosen.Glow, Panel);
    if (Left > At + 6) tui::PutText(Out, core::Point{Left, Row}, Pot, PotWide, Brewing.empty() ? Chosen.Quiet : Chosen.Text, Panel);
}

void Tower::PaintHints(tui::Frame &Out) const
{
    const core::Size Size = Screen.Size();
    const std::int32_t Row = Size.Height - 1;
    Out.Fill(core::Rect{0, Row, Size.Width, 1}, ' ', {Chosen.Text, Chosen.Bars});
    std::vector<Hint> Hints;
    if (Reading.Open) {
        Hints = {{"tab", "page", 1, false}, {"\xE2\x86\x95", "choose", 2, false}, {"esc", "close", 0, false}};
        if (Reading.Page == 1) Hints.push_back({"enter", "cast it", 3, true});
    } else {
        Hints = {{"\xE2\x86\x94", "choose", 1, false},
                 {"space", "put in", 2, false},
                 {"enter", "drink", 3, !Brewing.empty()},
                 {"backspace", "take out", 7, false},
                 {Spelled(KeySpell), "spell", 5, false},
                 {Spelled(KeyBook), "grimoire", 6, false},
                 {Spelled(KeyHelp), "keys", 8, false},
                 {"esc", "leave", 4, false}};
    }
    std::string Right;
    if (LaidUntilMs != 0) Right = std::format("{} lifts it", Spelled(KeyLift));
    std::int32_t Budget = Size.Width - 2 - (Right.empty() ? 0 : static_cast<std::int32_t>(tui::CellsOf(Right)) + 3);
    std::vector<bool> Shown(Hints.size(), false);
    for (int Rank = 0; Rank <= 8; Rank++) {
        for (std::size_t At = 0; At < Hints.size(); At++) {
            if (Hints[At].Rank != Rank) continue;
            const auto Wide = static_cast<std::int32_t>(tui::CellsOf(Hints[At].Key) + 1 + tui::CellsOf(Hints[At].Does)) + 2;
            if (Wide <= Budget) {
                Shown[At] = true;
                Budget -= Wide;
            }
        }
    }
    std::int32_t Column = 1;
    for (std::size_t At = 0; At < Hints.size(); At++) {
        if (!Shown[At]) continue;
        Column += tui::PutText(Out, core::Point{Column, Row}, Hints[At].Key, Size.Width - Column, Hints[At].Urgent ? Chosen.Glow : Chosen.Title, Chosen.Bars);
        Column += 1 + tui::PutText(Out, core::Point{Column + 1, Row}, Hints[At].Does, Size.Width - Column - 1, Chosen.Quiet, Chosen.Bars) + 2;
    }
    if (!Right.empty()) {
        const auto Wide = static_cast<std::int32_t>(tui::CellsOf(Right));
        tui::PutText(Out, core::Point{Size.Width - Wide - 1, Row}, Right, Wide, Chosen.Glow, Chosen.Bars);
    }
}

void Tower::PaintHelp(tui::Frame &Out) const
{
    std::string Charmed, Things;
    for (const Charm &One : Charms()) Charmed += (Charmed.empty() ? "" : ", ") + std::string(One.Word);
    for (const Ingredient &One : Ingredients()) Things += (Things.empty() ? "" : ", ") + std::string(One.Word);
    const std::pair<std::string, std::string> Keys[] = {
        {"left, right", "choose a thing on the shelf; home and end go to the first and the last"},
        {"space", "drop the chosen thing in the pot, four at most; the liquid takes its color"},
        {"enter", "drink the brew: what is in the pot decides what the glass does"},
        {"backspace", "take the last thing out again"},
        {Spelled(KeySpell), "cast a spell chosen by chance"},
        {Spelled(KeyBook), "the grimoire: potions found and spells known; enter casts the spell chosen"},
        {Spelled(KeyEmpty), "pour the pot out"},
        {Spelled(KeyLift), "lift what lies over the glass"},
        {Spelled(KeyDrink), "drink, as enter does"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "leave: what lies over the glass goes with the wizard"},
        {"wizard <charm>", "cast that charm at once: " + Charmed},
        {"wizard <things>", "mix and drink them at once: " + Things},
    };
    constexpr std::int32_t KeyWidth = 18;
    const core::Size Size = Screen.Size();
    const std::int32_t Wide = std::min(Size.Width - 2, 78);
    const auto Room = static_cast<std::size_t>(Wide - KeyWidth - 5);
    std::int32_t Lines = 0;
    for (const auto &[Key, Does] : Keys) Lines += static_cast<std::int32_t>(Wrapped(Does, Room).size());
    const core::Rect Area = tui::Centred(Size, Wide, std::min(Size.Height - 1, Lines + 4));
    const core::Rect Inside = tui::DrawWindow(Out, Area, "the wizard's keys", "settings wizard keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        tui::PutText(Out, core::Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        for (const std::string &Line : Wrapped(Does, Room)) {
            if (Row >= Inside.Y + Inside.Height) return;
            tui::PutText(Out, core::Point{Inside.X + 2 + KeyWidth, Row++}, Line, Inside.Width - KeyWidth - 3, tui::WindowInk, tui::WindowPaper);
        }
    }
}

}  // namespace wizard
