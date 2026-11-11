/* keys.cpp — the chosen keys, the leave question and the keys panel (keys.h). */

#include "keys.h"

#include <algorithm>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::int32_t KeyColumn = 24;

}  // namespace

bool Chose(const Look &Shown, SeaKey Which, const tui::KeyEvent &Pressed)
{
    const kb_event_t Raw{Pressed.Scancode, Pressed.Ascii, Pressed.Mods};
    return KeyChordMatches(&Shown.Keys[Which], &Raw);
}

std::string Spelled(const Look &Shown, SeaKey Which)
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Which], Spelling, sizeof(Spelling));
    return Spelling;
}

LeavePrompt::Verdict LeavePrompt::Answer(const tui::KeyEvent &Pressed)
{
    if (Pressed.IsExtended() || Pressed.IsCtrl() || Pressed.IsAlt()) return Verdict::Asking;
    if (Pressed.Ascii == tui::KeyCodes::Enter) return Verdict::Leave;
    if (Pressed.Ascii == tui::KeyCodes::Escape) {
        Asking = false;
        return Verdict::Stay;
    }
    return Verdict::Asking;
}

void LeavePrompt::Paint(tui::Frame &Out, const Look &Shown) const
{
    if (!Asking) return;
    const core::Rect Area = tui::Centred(Out.Size(), 44, 7);
    const core::Rect Inside = PaintPanel(Out, Shown, Area, "LEAVE THE GAME?", "enter leave   esc stay");
    const Color      Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 1, "The game in progress will be lost.", Shown.Text, Paper);
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 3, "Enter leaves it, esc plays on.", Shown.Quiet, Paper);
}

void PaintKeys(tui::Frame &Out, const Look &Shown, std::string_view Title, std::span<const KeyLine> Lines)
{
    const core::Size Size = Out.Size();
    const std::int32_t Wide = std::min(Size.Width - 4, 76);
    const auto Room = static_cast<std::size_t>(Wide - KeyColumn - 5);
    std::int32_t Total = 0;
    for (const KeyLine &Line : Lines) Total += static_cast<std::int32_t>(Wrapped(Line.second, Room).size());
    const core::Rect Inside = PaintPanel(Out, Shown, tui::Centred(Size, Wide, std::min(Size.Height - 2, Total + 4)), Title, "any key closes this");
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    std::int32_t Row = Inside.Y + 1;
    for (const KeyLine &Line : Lines) {
        tui::PutText(Out, core::Point{Inside.X + 1, Row}, Line.first, KeyColumn, Shown.Title, Paper);
        for (const std::string &Words : Wrapped(Line.second, Room)) {
            if (Row >= Inside.Y + Inside.Height) return;
            tui::PutText(Out, core::Point{Inside.X + 1 + KeyColumn, Row++}, Words, Inside.Width - KeyColumn - 2, Shown.Text, Paper);
        }
    }
}

}  // namespace seabattle
