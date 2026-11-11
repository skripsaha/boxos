/* book.cpp — the grimoire open on the glass: a list of the potions (the ones
 * not yet found only as question marks and a clue) or of the spells, and what
 * the one chosen is — its things drawn as they are on the shelf, what it does
 * to the glass, what to type to have it. */

#include <algorithm>
#include <format>
#include <string>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "icons.h"
#include "speech.h"
#include "tower.h"

namespace wizard {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

// The rows of a list of Count that fit in Rows around the one at Cursor: the first of them.
int FirstOf(int Count, int Rows, int Cursor)
{
    return std::clamp(Cursor - Rows / 2, 0, std::max(0, Count - Rows));
}

}  // namespace

void Tower::PaintBook(tui::Frame &Out) const
{
    const core::Size Size = Screen.Size();
    const std::int32_t Wide = std::min(Size.Width - 2, 78);
    const core::Rect Area = tui::Centred(Size, Wide, std::min(Size.Height - 2, 22));
    const core::Rect Inside = tui::DrawWindow(Out, Area, "the grimoire", std::format("{} of {} potions found", Book.KnownCount(), RecipeCount),
                                              "tab turns the page");
    const bool Spells = Reading.Page == 1;
    const int Count = static_cast<int>(Spells ? CharmCount : RecipeCount);
    const int Cursor = Reading.Cursor[Reading.Page];
    const std::int32_t ListWide = std::min<std::int32_t>(30, Inside.Width / 2 - 1);

    tui::PutText(Out, core::Point{Inside.X + 2, Inside.Y}, "potions", 8, Spells ? tui::HintInk : tui::TitleInk, tui::WindowPaper);
    tui::PutText(Out, core::Point{Inside.X + 11, Inside.Y}, "spells", 7, Spells ? tui::TitleInk : tui::HintInk, tui::WindowPaper);
    const std::int32_t Rows = Inside.Height - 2;
    const int First = FirstOf(Count, Rows, Cursor);
    for (std::int32_t Row = 0; Row < Rows && First + Row < Count; Row++) {
        const int At = First + Row;
        const bool Here = At == Cursor;
        std::string Line;
        if (Spells) Line = std::string(Charms()[static_cast<std::size_t>(At)].Name);
        else Line = Book.Knows(static_cast<std::size_t>(At)) ? std::string(Recipes()[static_cast<std::size_t>(At)].Name) : "? ? ? ?";
        const core::Color Paper = Here ? tui::ChosenPaper : tui::WindowPaper;
        Out.Fill(core::Rect{Inside.X + 1, Inside.Y + 1 + Row, ListWide, 1}, ' ', {tui::WindowInk, Paper});
        tui::PutText(Out, core::Point{Inside.X + 2, Inside.Y + 1 + Row}, Line, ListWide - 2, Here ? tui::TitleInk : tui::WindowInk, Paper);
    }

    const std::int32_t Left = Inside.X + ListWide + 3, Room = Inside.Width - ListWide - 5;
    std::int32_t Row = Inside.Y + 1;
    const auto Say = [&](const std::string &Text, core::Color Ink) {
        for (const std::string &Line : Wrapped(Text, static_cast<std::size_t>(Room))) {
            if (Row >= Inside.Y + Inside.Height) return;
            tui::PutText(Out, core::Point{Left, Row++}, Line, Room, Ink, tui::WindowPaper);
        }
    };
    if (Spells) {
        const Charm &One = Charms()[static_cast<std::size_t>(Cursor)];
        Say(std::string(One.Name), tui::TitleInk);
        Say(std::format("ask for it: wizard {}", One.Word), tui::WindowInk);
        Say(std::format("it does: {}", EffectNames(One.Kinds)), tui::WindowInk);
        Say(std::string("enter casts it now"), tui::HintInk);
    } else if (Book.Knows(static_cast<std::size_t>(Cursor))) {
        const Recipe &One = Recipes()[static_cast<std::size_t>(Cursor)];
        Say(std::string(One.Name), tui::TitleInk);
        Row++;
        std::int32_t Place = Left;
        for (std::uint8_t Part : One.Of) {
            if (Part == NoThing) continue;
            DrawIconOnFrame(Out, Place, Row * 2, Part);
            tui::PutText(Out, core::Point{Place, Row + IconSize / 2}, std::string(Ingredients()[Part].Name), IconSize + 1, tui::WindowInk, tui::WindowPaper);
            Place += IconSize + 2;
        }
        Row += IconSize / 2 + 2;
        std::uint32_t Kinds = One.Kinds;
        for (std::uint8_t Part : One.Of)
            if (Part != NoThing) Kinds |= Ingredients()[Part].Kinds;
        Say(std::format("it does: {}", EffectNames(Kinds)), tui::WindowInk);
        Say(std::string(One.Verse), tui::HintInk);
    } else {
        const Recipe &One = Recipes()[static_cast<std::size_t>(Cursor)];
        const auto Parts = std::count_if(One.Of.begin(), One.Of.end(), [](std::uint8_t Part) { return Part != NoThing; });
        Say("Not found yet.", tui::TitleInk);
        Say(std::format("It is made of {} things, in any order: {}.", Parts, One.Clue), tui::WindowInk);
        Say("Brew them together and drink the pot, and it is written here.", tui::HintInk);
    }
    const std::string Counts = std::format("drunk {}   cast {}", Book.Drunk(), Book.Cast());
    tui::PutText(Out, core::Point{Left, Inside.Y + Inside.Height - 1}, Counts, Room, tui::HintInk, tui::WindowPaper);
}

}  // namespace wizard
