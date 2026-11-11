/* panels.cpp — the key list and the secret word's entry (panels.h). */

#include "panels.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "round.h"

namespace hangman {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

std::string Spelled(const KeyChord &Chord)
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chord, Spelling, sizeof(Spelling));
    return Spelling;
}

constexpr std::int32_t KeyColumn = 12;

}  // namespace

void PaintHelp(tui::Frame &Out, const Look &Shown, HintCost Price)
{
    const std::string Hint = Price == HintCost::Points ? std::format("take a hint: a letter is given ({} points off)", HintPoints)
                                                       : "take a hint: a letter is given (costs a mistake)";
    const std::pair<std::string, std::string> Lines[] = {
        {"a - z", "guess a letter; one already said is only refused"},
        {Spelled(Shown.Keys[KeyHint]), Hint},
        {Spelled(Shown.Keys[KeyFresh]), "give this word up and take another"},
        {Spelled(Shown.Keys[KeyFriend]), "a friend types a secret word for you to guess"},
        {Spelled(Shown.Keys[KeyHelp]), "this list"},
        {"enter", "the next word, once the round is over"},
        {"esc", "leave"},
    };
    const core::Size Size = Out.Size();
    const std::int32_t Wide = std::min(Size.Width - 2, 70);
    const core::Rect Inside = tui::DrawWindow(Out, tui::Centred(Size, Wide, static_cast<std::int32_t>(std::size(Lines)) + 4), "KEYS", "", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &Line : Lines) {
        tui::PutText(Out, {Inside.X + 1, Row}, Line.first, KeyColumn, tui::TitleInk, tui::WindowPaper);
        tui::PutText(Out, {Inside.X + 1 + KeyColumn, Row}, Line.second, Inside.Width - KeyColumn - 2, tui::WindowInk, tui::WindowPaper);
        Row++;
    }
}

SecretEntry::Verdict SecretEntry::Key(const tui::KeyEvent &Pressed)
{
    if (Pressed.IsExtended() || Pressed.IsCtrl() || Pressed.IsAlt()) return Verdict::Typing;
    if (Pressed.Ascii == tui::KeyCodes::Escape) return Verdict::Cancelled;
    if (Pressed.Ascii == tui::KeyCodes::Enter) return Typed.size() >= LeastSecret ? Verdict::Done : Verdict::Typing;
    if (Pressed.Ascii == tui::KeyCodes::Backspace) {
        if (!Typed.empty()) Typed.pop_back();
    } else if (Round::IsLetter(Pressed.Ascii) && Typed.size() < MostSecret) {
        Typed += Round::Upper(Pressed.Ascii);
    }
    return Verdict::Typing;
}

void SecretEntry::Paint(tui::Frame &Out) const
{
    const core::Size Size = Out.Size();
    const core::Rect Inside = tui::DrawWindow(Out, tui::Centred(Size, std::min(Size.Width - 2, 54), 9), "A FRIEND'S WORD", "",
                                              "enter plays   backspace erases   esc cancels");
    const auto Say = [&](std::int32_t Row, std::string_view Words, core::Color Ink) {
        tui::PutText(Out, {Inside.X + 1, Inside.Y + Row}, Words, Inside.Width - 2, Ink, tui::WindowPaper);
    };
    Say(1, std::format("Type a secret word of {} to {} letters.", LeastSecret, MostSecret), tui::WindowInk);
    Say(2, "What is typed is shown only as stars.", tui::HintInk);
    std::string Stars;
    for (std::size_t At = 0; At < MostSecret; At++) Stars += At < Typed.size() ? "* " : "_ ";
    Say(4, Stars, tui::TitleInk);
    Say(5, Typed.size() >= LeastSecret ? "Enter hands the glass to the one who guesses." : "A little longer, please.", tui::HintInk);
}

}  // namespace hangman
