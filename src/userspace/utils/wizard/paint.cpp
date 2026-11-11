/* paint.cpp — the tower as a picture: where things stand, the room and its
 * people (art's), and the words around them. */

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "tower.h"

namespace wizard {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::int32_t BarRows = 1;
constexpr std::int32_t SpeechRows = 3;
constexpr std::int32_t ShelfRows = 4;      // two of things, the cauldron's line, the keys
constexpr std::int32_t LeastWidth = 80;    // what the art is made for

void Say(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, Color Ink, Color Under)
{
    tui::PutText(Out, At, Text, Width, Ink, Under);
}

// Words broken into lines no wider than Width cells, at spaces.
std::vector<std::string> Wrapped(std::string_view Words, std::size_t Width)
{
    std::vector<std::string> Lines(1);
    while (!Words.empty()) {
        const std::size_t End = Words.find(' ');
        const std::string_view Word = Words.substr(0, End);
        if (!Lines.back().empty() && tui::CellsOf(Lines.back()) + 1 + tui::CellsOf(Word) > Width) Lines.emplace_back();
        if (!Lines.back().empty()) Lines.back() += ' ';
        Lines.back() += Word;
        Words = End == std::string_view::npos ? std::string_view() : Words.substr(End + 1);
    }
    return Lines;
}

}  // namespace

Scene Tower::Placed() const
{
    const core::Size Size = Screen.Size();
    Scene Where{};
    Where.ArtTopRow = BarRows + SpeechRows;
    Where.ArtRows = std::max(1, Size.Height - Where.ArtTopRow - ShelfRows);
    Where.Scale = std::max(1, std::min(Where.ArtRows * 2 / WizardHeight, Size.Width / LeastWidth));
    Where.Size = core::Size{Size.Width / Where.Scale, Where.ArtRows * 2 / Where.Scale};
    // The floor stands at the bottom of the art, however it divides.
    Where.OriginY = Where.ArtTopRow * 2 + Where.ArtRows * 2 - Where.Size.Height * Where.Scale;
    Where.WizardX = std::max(1, Where.Size.Width / 4 - 15);
    Where.WizardY = Where.Size.Height - WizardHeight;
    Where.CauldronX = Where.Size.Width * 3 / 4 - CauldronWidth / 2;
    Where.CauldronY = Where.Size.Height - CauldronHeight - 1;
    return Where;
}

void Tower::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({Shown.Text, Shown.Room});
    const Scene Where = Placed();
    const Pen   At{&Out, core::Point{0, Where.OriginY}, Where.Scale, Where.Size};

    DrawRoom(At, Shown, LastMs);
    DrawCauldron(At, Where.CauldronX, Where.CauldronY, Liquid(Mixed), static_cast<std::uint32_t>(Mixed.size()), LastMs);
    DrawWizard(At, Where.WizardX, Where.WizardY, Shown, LastMs, Excitement);
    Sparks.Draw(At);

    const core::Size Size = Screen.Size();
    PaintBars(Out);
    PaintSpeech(Out, core::Rect{1, BarRows, Size.Width - 2, SpeechRows});
    PaintShelf(Out, core::Rect{0, Size.Height - ShelfRows, Size.Width, ShelfRows});
    if (Help) PaintHelp(Out);
}

void Tower::PaintBars(tui::Frame &Out) const
{
    const core::Size Size = Screen.Size();
    Out.Fill(core::Rect{0, 0, Size.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    Say(Out, core::Point{1, 0}, "wizard", Size.Width - 2, Shown.Title, Shown.Bars);
    const std::string Right = LaidUntilMs != 0 && Spoken
                                  ? std::format("{}  {} s ", Spoken->Name, LeftSeconds())
                                  : std::string("nothing lies over the glass ");
    const auto Wide = static_cast<std::int32_t>(tui::CellsOf(Right));
    Say(Out, core::Point{std::max(9, Size.Width - Wide), 0}, Right, Wide, LaidUntilMs != 0 ? Shown.Glow : Shown.Quiet,
        Shown.Bars);
}

void Tower::PaintSpeech(tui::Frame &Out, core::Rect Area) const
{
    Out.Fill(Area, ' ', {Shown.Ink, Shown.Paper});
    const auto Width = static_cast<std::size_t>(Area.Width - 2);
    if (!Spoken) {
        const std::string Greeting = std::format(
            "Ah, a visitor! Press {} for a spell, or put things in the cauldron with 1 to 8 and drink them with {}.",
            Spelled(KeySpell), Spelled(KeyDrink));
        const std::vector<std::string> Lines = Wrapped(Greeting, Width);
        Say(Out, core::Point{Area.X + 1, Area.Y}, "The Old Wizard", Area.Width - 2, Shown.Ink, Shown.Paper);
        for (std::size_t Line = 0; Line < Lines.size() && Line < 2; Line++)
            Say(Out, core::Point{Area.X + 1, Area.Y + 1 + static_cast<std::int32_t>(Line)}, Lines[Line], Area.Width - 2,
                Shown.Ink, Shown.Paper);
        return;
    }
    Say(Out, core::Point{Area.X + 1, Area.Y}, Spoken->Name, Area.Width - 2, Shown.Ink, Shown.Paper);
    const std::vector<std::string> Lines = Wrapped(Spoken->Verse, Width);
    for (std::size_t Line = 0; Line < Lines.size() && Line < 2; Line++)
        Say(Out, core::Point{Area.X + 1, Area.Y + 1 + static_cast<std::int32_t>(Line)}, Lines[Line], Area.Width - 2,
            Shown.Ink, Shown.Paper);
}

void Tower::PaintShelf(tui::Frame &Out, core::Rect Area) const
{
    Out.Fill(Area, ' ', {Shown.Text, Shown.Bars});
    const std::int32_t Cell = Area.Width / 4;
    for (std::size_t At = 0; At < IngredientCount; At++) {
        const Ingredient &One = Ingredients()[At];
        const auto Count = static_cast<std::int32_t>(std::count(Mixed.begin(), Mixed.end(), static_cast<std::uint8_t>(At)));
        const core::Point Here{Area.X + 1 + static_cast<std::int32_t>(At % 4) * Cell, Area.Y + static_cast<std::int32_t>(At / 4)};
        Out.Put(Here, tui::Glyphs::Full, {Color::FromRaw(One.Color), Shown.Bars});
        const std::string Label = Count > 0 ? std::format("{} {} x{}", At + 1, One.Name, Count)
                                            : std::format("{} {}", At + 1, One.Name);
        Say(Out, core::Point{Here.X + 2, Here.Y}, Label, Cell - 3, Count > 0 ? Shown.Title : Shown.Text, Shown.Bars);
    }

    std::string Inside;
    for (std::uint8_t One : Mixed) Inside += (Inside.empty() ? "" : " + ") + std::string(Ingredients()[One].Name);
    const std::string Line = Mixed.empty() ? std::string("The cauldron is empty")
                                           : std::format("In the cauldron: {}", Inside);
    Say(Out, core::Point{Area.X + 1, Area.Y + 2}, Line, Area.Width - 2, Mixed.empty() ? Shown.Quiet : Shown.Text, Shown.Bars);

    const bool Speaking = StatusUntilMs > LastMs && !Status.empty();
    const std::string Keys = std::format("{} spell   {} drink   {} pour out   {} lift   {} keys   esc leave",
                                         Spelled(KeySpell), Spelled(KeyDrink), Spelled(KeyEmpty), Spelled(KeyLift),
                                         Spelled(KeyHelp));
    Say(Out, core::Point{Area.X + 1, Area.Y + 3}, Speaking ? Status : Keys, Area.Width - 2,
        Speaking ? Shown.Glow : Shown.Quiet, Shown.Bars);
}

void Tower::PaintHelp(tui::Frame &Out) const
{
    std::string Charmed, Things;
    for (const Charm &One : Charms()) Charmed += (Charmed.empty() ? "" : ", ") + std::string(One.Word);
    for (const Ingredient &One : Ingredients()) Things += (Things.empty() ? "" : ", ") + std::string(One.Word);
    const std::pair<std::string, std::string> Keys[] = {
        {"1 - 8", "put a thing in the cauldron, four at most"},
        {"backspace", "take the last one out again"},
        {Spelled(KeySpell), "cast a spell: a charm chosen at random"},
        {Spelled(KeyDrink), "drink what is in the cauldron: the mix decides what it does"},
        {Spelled(KeyEmpty), "pour the cauldron out"},
        {Spelled(KeyLift), "lift what lies over the glass"},
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
    const core::Rect Area = tui::Centred(Size, Wide, Lines + 4);
    const core::Rect Inside = tui::DrawWindow(Out, Area, "the wizard's keys", "settings wizard keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        Say(Out, core::Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        for (const std::string &Line : Wrapped(Does, Room)) {
            if (Row >= Inside.Y + Inside.Height) return;
            Say(Out, core::Point{Inside.X + 2 + KeyWidth, Row++}, Line, Inside.Width - KeyWidth - 3, tui::WindowInk,
                tui::WindowPaper);
        }
    }
}

}  // namespace wizard
