/* game.cpp — the run of flips, and the keys. */

#include "game.h"

#include <utility>

#include "box/clock.h"

namespace coinflip {

namespace {

namespace tui = painter::tui;

// How long a coin lies before the next flip of a run is tossed.
constexpr std::uint64_t HoldUs = 350000;
// How long the screen waits for a key: while the coin moves, one frame; while
// it lies, only as long as a changed setting may go unseen.
constexpr std::uint32_t FrameMs = 33;
constexpr std::uint32_t IdleMs  = 250;

}  // namespace

Game::Game(tui::Stage Opened, Plan Wanted) : Screen(std::move(Opened)), Shown(ReadLook()), Wish(Wanted) {}

void Game::Run()
{
    Screen.HearEnd();
    StartRun(clock_uptime_us());
    for (;;) {
        if (Watch.changed()) Shown = ReadLook();
        const std::uint64_t Now = clock_uptime_us();
        Advance(Now);
        Paint(Now);
        if (!Screen.Present()) return;

        const tui::Event Heard = Screen.Wait(Flying() || Running() ? FrameMs : IdleMs);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return;
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key, clock_uptime_us())) return;
    }
}

void Game::StartRun(std::uint64_t Now)
{
    Left     = Wish.Count;
    Done     = 0;
    LandedUs = Now - HoldUs;
}

// The face is drawn here, before the coin leaves the hand.
void Game::Begin(std::uint64_t Now)
{
    Landing = Chance.Draw();
    Current.emplace(Landing, Shown, Wish.Count > 1);
    StartedUs = Now;
    Aloft     = true;
}

void Game::Record(Face Landed)
{
    Counted.Add(Landed, Wish.Called);
    Left--;
    Done++;
}

void Game::Land(std::uint64_t Now)
{
    Aloft    = false;
    LandedUs = Now;
    Record(Landing);
}

// The rest of the run at once: the flip in the air lands, the others are drawn
// and counted without being shown, and the last lies as if it had been tossed.
void Game::Hurry(std::uint64_t Now)
{
    if (Aloft) Land(Now);
    while (Left > 0) {
        Landing = Chance.Draw();
        Record(Landing);
    }
    Current.emplace(Landing, Shown, true);
    LandedUs = Now;
}

void Game::Advance(std::uint64_t Now)
{
    if (Aloft) {
        if (static_cast<double>(Now - StartedUs) >= Current->Length() * 1e6) Land(Now);
    } else if (Running() && Now - LandedUs >= HoldUs) {
        Begin(Now);
    }
}

bool Game::OnKey(const tui::KeyEvent &Key, std::uint64_t Now)
{
    if (Help) {
        Help = false;
        return true;
    }
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](CoinKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Help = true;
        return true;
    }
    const bool Plain = !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
    const bool Flip  = Is(KeyFlip) || (Plain && (Key.Ascii == ' ' || Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r'));
    if (Flip) {
        if (!Running()) StartRun(Now);
        else if (Wish.Count > 1) Hurry(Now);
        return true;
    }
    // A call is a chosen chord, or plain h or t.
    std::optional<Face> Called;
    if (Is(KeyHeads) || (Plain && (Key.Ascii == 'h' || Key.Ascii == 'H'))) Called = Face::Heads;
    if (Is(KeyTails) || (Plain && (Key.Ascii == 't' || Key.Ascii == 'T'))) Called = Face::Tails;
    if (Called && !Running()) {
        Wish.Called = Called;
        StartRun(Now);
        return true;
    }
    if (Plain && Key.Ascii == tui::KeyCodes::Backspace && !Running()) Wish.Called.reset();
    return !(Plain && Key.Ascii == tui::KeyCodes::Escape);
}

}  // namespace coinflip
