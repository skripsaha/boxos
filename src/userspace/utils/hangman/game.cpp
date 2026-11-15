/* game.cpp — the loop, the keys and how the glass is laid out (game.h). */

#include "game.h"

#include <algorithm>
#include <format>
#include <utility>

#include "box/clock.h"

namespace hangman {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

// How often the picture is drawn while something is moving that the person watches, and while only
// the clouds and the leaves drift.
constexpr std::uint32_t FrameMs = 66;
constexpr std::uint32_t CalmMs = 125;
constexpr std::uint64_t BoardMs = 2000;   // how long after a letter, a refusal or the end the board's own motion lasts

// The margins round the scene and the words, and the gap between them; a glass of the text console's
// height gives the bars a row each and nothing more.
constexpr std::int32_t Margin = 1;
constexpr std::int32_t Gap = 2;
constexpr std::int32_t TallRows = 30;

}  // namespace

Game::Game(tui::Stage Opened, Request Wanted)
    : Screen(std::move(Opened)), Chosen(ReadLook()), Asked(Wanted)
{
}

void Game::Run()
{
    Screen.HearEnd();
    std::uint64_t Last = clock_uptime_ms();
    Time = Last;
    Next();
    if (!Book.Trouble().empty()) Tell(Book.Trouble(), NoteKind::Bad);
    for (;;) {
        if (Watch.changed()) Chosen = ReadLook();
        Time = clock_uptime_ms();
        const core::Size Size = Screen.Size();
        Picture.Step(static_cast<std::uint32_t>(Time - Last), Time, Chance, Chosen, SceneCells(Size));
        Last = Time;
        Paint();
        if (!Screen.Present()) break;

        tui::Event Heard = Screen.Wait(Moving() ? FrameMs : CalmMs);
        while (Heard.Kind == tui::EventKind::Key) {
            Time = clock_uptime_ms();
            if (!OnKey(Heard.Key)) return;
            Heard = Screen.Poll();
        }
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) break;
    }
}

bool Game::Chose(HangKey Which, const tui::KeyEvent &Pressed) const
{
    const kb_event_t Raw{Pressed.Character, Pressed.Scancode, Pressed.Mods};
    return KeyChordMatches(&Chosen.Keys[Which], &Raw);
}

std::string Game::Spelled(HangKey Which) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chosen.Keys[Which], Spelling, sizeof(Spelling));
    return Spelling;
}

// False when the person has asked to leave.
bool Game::OnKey(const tui::KeyEvent &Pressed)
{
    Chance.Stir(clock_uptime_us());
    if (Window == Overlay::Help) {
        Window = Overlay::None;
        return true;
    }
    if (Window == Overlay::Secret) {
        switch (Secret.Key(Pressed)) {
        case SecretEntry::Verdict::Typing:    break;
        case SecretEntry::Verdict::Cancelled: Window = Overlay::None; Secret.Clear(); break;
        case SecretEntry::Verdict::Done:
            Window = Overlay::None;
            if (Now->Touched() && Now->State() == Phase::Playing) GiveUp();
            Begin(Secret.Word(), "a friend's word", false);
            Secret.Clear();
            break;
        }
        return true;
    }
    if (Chose(KeyHelp, Pressed)) Window = Overlay::Help;
    else if (Chose(KeyHint, Pressed)) TakeHint();
    else if (Chose(KeyFresh, Pressed)) {
        if (Now->State() == Phase::Playing && Now->Touched()) GiveUp();
        else Next();
    } else if (Chose(KeyFriend, Pressed)) Window = Overlay::Secret;
    else if (!Pressed.IsExtended() && !Pressed.IsCtrl() && !Pressed.IsAlt()) return OnPlainKey(Pressed);
    return true;
}

bool Game::OnPlainKey(const tui::KeyEvent &Pressed)
{
    if (Pressed.Character == tui::KeyCodes::Escape) return false;
    if (Pressed.Character == tui::KeyCodes::Enter && Now->State() != Phase::Playing) Next();
    else if (Round::IsLetter(Pressed.Character)) Say(static_cast<char>(Pressed.Character));
    return true;
}

bool Game::Moving() const
{
    if (Picture.Busy(Time) || (EndedAt != 0 && Time - EndedAt < BoardMs) || (Refused != 0 && Time - RefusedAt < BoardMs)) return true;
    return std::any_of(Lit.begin(), Lit.end(), [&](std::uint64_t Moment) { return Moment + BoardMs > Time; });
}

core::Rect Game::SceneCells(core::Size Size) const
{
    const std::int32_t Edge = Size.Height < TallRows ? 1 : 2;
    const std::int32_t Rows = std::max(1, Size.Height - Edge * 2);
    const std::int32_t Fair = static_cast<std::int32_t>(static_cast<float>(Rows) * 2.2f);
    const std::int32_t Wide = std::min((Size.Width - Margin * 2 - Gap) * 50 / 100, Fair);
    return {Margin, Edge, std::max(1, Wide), Rows};
}

void Game::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    const core::Size Size = Out.Size();
    Out.Clear({Chosen.Text, PaperOf(Chosen)});
    Out.SetCaret(std::nullopt);

    const core::Rect Cells = SceneCells(Size);
    Picture.Paint(Out, Cells, Chosen, Time);
    const std::int32_t Beside = Cells.X + Cells.Width + Gap;
    const core::Rect Panel{Beside, Cells.Y, Size.Width - Beside - Margin, Cells.Height};
    const std::string HintKey = Spelled(KeyHint);
    const Round &Standing = *Now;
    const std::uint32_t Left = Chosen.HintsAllowed > static_cast<std::uint32_t>(Standing.Hints()) ? Chosen.HintsAllowed - static_cast<std::uint32_t>(Standing.Hints()) : 0;
    PaintBoard(Out, Panel, Facts{.Playing = Standing,
                                 .Shown = Chosen,
                                 .Total = Book.Totals(),
                                 .Lit = Lit,
                                 .Given = Given,
                                 .Now = Time,
                                 .EndedAt = EndedAt,
                                 .Earned = Earned,
                                 .Counted = Counted,
                                 .Refused = Refused,
                                 .RefusedAt = RefusedAt,
                                 .Note = Note,
                                 .NoteWas = NoteWas,
                                 .NoteAt = NoteAt,
                                 .HintsLeft = Left,
                                 .HintKey = HintKey});
    PaintTopBar(Out, Chosen);
    const bool Over = Standing.State() != Phase::Playing;
    PaintHintBar(Out, Chosen, std::format("{}  {} hint  {} new  {} friend  {} keys  esc leave",
                                          Over ? "enter next word" : "a-z guess", HintKey, Spelled(KeyFresh), Spelled(KeyFriend), Spelled(KeyHelp)));
    if (Window == Overlay::Help) PaintHelp(Out, Chosen, Chosen.Price);
    if (Window == Overlay::Secret) Secret.Paint(Out);
}

}  // namespace hangman
