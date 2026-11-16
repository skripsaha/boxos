/* game.cpp — the table's loop: a frame while the dice tumble and none once
 * they have settled, the keys, and the bars around the table. */

#include "game.h"

#include <algorithm>
#include <format>

#include "box/clock.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

namespace dice {

namespace {

namespace tui = painter::tui;
using painter::core::Rect;

// Rows around the table and the panel: the title above, the line typed in and
// the line of keys below.
constexpr int BarRows = 3;
// The most of what is typed: a roll is never longer.
constexpr std::size_t MostTyped = 48;
// While nothing moves, the settings are looked at this often.
constexpr std::uint32_t IdleMs = 250;

const char *Describe(Entropy Kind)
{
    switch (Kind) {
    case Entropy::Seed: return "the chip's own seed entropy";
    case Entropy::Rand: return "the chip's own random generator";
    default:            return "this chip gives none: a generator stirred by the cycle counter";
    }
}

}  // namespace

Game::Game(tui::Stage Opened, Source &From, Expression Start, std::string Notice)
    : Screen(std::move(Opened)), Shown(ReadLook()), Dice(From), Current(std::move(Start)), Said(std::move(Notice))
{
}

void Game::Arrange()
{
    const painter::core::Size Glass  = Screen.Size();
    const int                 Middle = Glass.Height - BarRows;
    Content   = Compose(Thrown, Glass.Width, std::max(2, Middle / 2), Shown);
    PanelArea = {0, 1 + Middle - Content.Rows, Glass.Width, Content.Rows};
    TableArea = {0, 1, Glass.Width, Middle - Content.Rows};
}

void Game::Again()
{
    Thrown = dice::Throw(Current, Dice);
    Arrange();
    Board.Throw(Thrown, {TableArea.Width, TableArea.Height}, Dice);
    ThrownAtUs = clock_uptime_us();
    Dirty      = true;
}

void Game::Submit()
{
    if (Typed.empty()) {
        Again();
        return;
    }
    std::expected<Expression, ParseError> Wanted = Parse(Typed);
    if (!Wanted) {
        Said = Wanted.error().Message;
        return;
    }
    Current = std::move(*Wanted);
    Typed.clear();
    Again();
}

std::string Game::Spelled(DiceKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

bool Game::OnKey(const tui::KeyEvent &Key)
{
    Dirty = true;
    if (Help) {
        Help = false;
        return true;
    }
    Said.clear();
    const kb_event_t Raw{Key.Character, Key.Scancode, Key.Mods};
    if (KeyChordMatches(&Shown.Keys[KeyHelp], &Raw)) {
        Help = true;
        return true;
    }
    if (KeyChordMatches(&Shown.Keys[KeyRoll], &Raw)) {
        Again();
        return true;
    }
    if (Key.IsCtrl() || Key.IsAlt() || Key.IsExtended()) return true;

    switch (Key.Character) {
    case tui::KeyCodes::Escape:
        return false;
    case tui::KeyCodes::Enter:
    case '\r':
        Submit();
        return true;
    case tui::KeyCodes::Backspace:
        if (!Typed.empty()) Typed.pop_back();
        return true;
    case ' ':
        if (Typed.empty()) {
            Again();
            return true;
        }
        break;
    default:
        break;
    }
    if (Key.Character >= ' ' && Key.Character < 0x7F && Typed.size() < MostTyped) Typed += Key.Character;
    return true;
}

void Game::Run()
{
    Screen.HearEnd();
    Again();
    bool          WasMoving = true;
    std::uint64_t NextFrameUs = 0;
    for (;;) {
        if (Watch.changed()) {
            Shown = ReadLook();
            Arrange();
            Dirty = true;
        }
        const std::uint64_t Elapsed = (clock_uptime_us() - ThrownAtUs) / 1000;
        const bool          Moving  = !Table::Settled(Elapsed, Shown);
        if (Moving || WasMoving || Dirty) {
            Paint(Elapsed);
            if (!Screen.Present()) return;
            Dirty = false;
        }
        WasMoving = Moving;

        std::uint32_t WaitMs = IdleMs;
        if (Moving) {
            const std::uint64_t Now = clock_uptime_us();
            if (NextFrameUs <= Now) NextFrameUs = Now;
            NextFrameUs += 1000000u / std::max<std::uint32_t>(1, Shown.FramesPerSecond);
            WaitMs = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, (NextFrameUs - Now) / 1000u));
        }
        const tui::Event Heard = Screen.Wait(WaitMs);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return;
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key)) return;
    }
}

void Game::Paint(std::uint64_t ElapsedMs)
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({Shown.Text, Shown.Panel});
    if (Help) {
        PaintHelp(Out);
        return;
    }
    Board.Paint(Out, TableArea, ElapsedMs, Shown);
    PaintPanel(Out, PanelArea, Content, Thrown, Table::Settled(ElapsedMs, Shown), Shown);
    PaintBars(Out, Screen.Size().Height);
}

void Game::PaintBars(tui::Frame &Out, int Rows)
{
    const int Width = Out.Size().Width;
    Out.Fill({0, 0, Width, 1}, ' ', {Shown.Text, Shown.Bars});
    Out.Write({1, 0}, "dice", {Shown.Text, Shown.Bars});
    Out.Write({6, 0}, Spell(Current), {Shown.Body, Shown.Bars});

    const int Typing = Rows - 2;
    Out.Fill({0, Typing, Width, 1}, ' ', {Shown.Text, Shown.Panel});
    Out.Write({1, Typing}, "roll>", {Shown.Quiet, Shown.Panel});
    if (Typed.empty()) Out.Write({7, Typing}, Spell(Current), {Shown.Quiet, Shown.Panel});
    else               Out.Write({7, Typing}, Typed, {Shown.Text, Shown.Panel});
    Out.SetCaret(painter::core::Point{7 + static_cast<int>(Typed.size()), Typing});
    const std::string Random = std::format("random: {} ", Name(Dice.Used()));
    Out.Write({std::max(0, Width - static_cast<int>(Random.size())), Typing}, Random, {Shown.Quiet, Shown.Panel});

    const int Keys = Rows - 1;
    Out.Fill({0, Keys, Width, 1}, ' ', {Shown.Text, Shown.Bars});
    if (!Said.empty()) {
        tui::PutText(Out, {1, Keys}, Said, Width - 2, Shown.Low, Shown.Bars);
        return;
    }
    const std::string Hints = std::format("enter, space: again   {}: again   {}: grammar and keys   esc: leave",
                                          Spelled(KeyRoll), Spelled(KeyHelp));
    tui::PutText(Out, {1, Keys}, Hints, Width - 2, Shown.Quiet, Shown.Bars);
}

void Game::PaintHelp(tui::Frame &Out)
{
    const int Width = Out.Size().Width;
    int       Row   = 1;
    const auto Say  = [&](const std::string &Text, Color Ink) {
        tui::PutText(Out, {2, Row}, Text, Width - 3, Ink, Shown.Panel);
        Row++;
    };
    Say("dice [what to roll]   rolls dice, several at once", Shown.Body);
    Row++;
    for (const std::string_view Line : GrammarLines()) Say(std::string(Line), Shown.Text);
    Row++;
    Say("type a roll, enter: roll it; enter or space on an empty line: again", Shown.Text);
    Say(std::format("{}: roll again    {}: this page    esc: leave", Spelled(KeyRoll), Spelled(KeyHelp)), Shown.Text);
    Row++;
    Say(std::format("random: {}, {}", Name(Dice.Used()), Describe(Dice.Used())), Shown.Quiet);
    Say("faces are unbiased: a draw that would favour low faces is thrown away", Shown.Quiet);
    Row++;
    Say("any key goes back to the table", Shown.Quiet);
}

}  // namespace dice
