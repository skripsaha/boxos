/* battle.cpp — a turn of the battle: aiming, firing, and what comes of each shot (battle.h). */

#include "battle.h"

#include <algorithm>
#include <format>
#include <numeric>

#include "hunter.h"
#include "pages.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;

constexpr std::uint32_t FlightMs = 720;
constexpr std::uint32_t MissMs = 880;
constexpr std::uint32_t HitMs = 1080;
constexpr std::uint32_t SinkMs = 1900;
constexpr std::uint32_t SettleMs = 520;
constexpr std::uint32_t HoldMs = 1150;
constexpr std::uint32_t WonMs = 1900;
constexpr std::uint32_t ThinkMs = 900;
constexpr std::uint32_t QuickMs = 380;          // what a pause is at the instant pace

std::string CellName(int Row, int Col) { return std::format("{}{}", static_cast<char>('A' + Col), Row + 1); }

}  // namespace

BattlePage::BattlePage(Context &Shared)
    : Page(Shared), Viewer(Shared.Viewer()), Place(ArenaFor(Shared.Screen.Size(), Shared.Game->Chosen().Grid))
{
    CursorRow = CursorCol = Game().Chosen().Grid / 2;
    if (Game().Shooting().Computer) {
        Said = std::format("{} is taking aim...", Game().Shooting().Name);
        Enter(Phase::Thinking, Paced(Ctx.Plan.Speed, ThinkMs));
        return;
    }
    Said = Ctx.Recap.empty() ? std::string("Choose where to fire.") : Ctx.Recap + " Choose where to fire.";
    Enter(Phase::Aim, 0);
}

void BattlePage::Enter(Phase Next, std::uint32_t Ms)
{
    Now = Next;
    Since = Ctx.Now;
    Span = Ms;
}

void BattlePage::Step(std::uint32_t ElapsedMs)
{
    (void)ElapsedMs;
    Fx.Prune(Ctx.Now);
    while (Now != Phase::Aim && Ctx.Now - Since >= Span) {
        switch (Now) {
        case Phase::Thinking: Aimed(); break;
        case Phase::Flight:   Land(); break;
        case Phase::Impact:   Impacted(); break;
        case Phase::Sinking:  Settle(); break;
        case Phase::Settle:   Next(); break;
        case Phase::Hold:     Close(); return;
        case Phase::Aim:      break;
        }
    }
}

void BattlePage::Aimed()
{
    const Aim Chosen = Hunt(Game().Aimed().Waters, Ctx.Chance);
    Fire(Chosen.Row, Chosen.Col);
}

void BattlePage::Fire(int Row, int Col)
{
    Before = Game().Aimed().Waters;
    Last = Game().Fire(Row, Col);
    const GridView &At = Target();
    const GridView &From = PersonShoots() ? Place.Own : Place.Aim;
    const std::uint32_t Flight = Paced(Ctx.Plan.Speed, FlightMs);
    Fx.Add(Effect{.Kind = EffectKind::Shell,
                  .X = static_cast<float>(At.CentreX(Col)),
                  .Y = static_cast<float>(At.CentreY(Row)),
                  .FromX = static_cast<float>(PersonShoots() ? From.X : From.X + From.Pixels()),
                  .FromY = static_cast<float>(From.Y + From.Pixels() / 2),
                  .Reach = static_cast<float>(At.Cell),
                  .Born = Ctx.Now,
                  .Life = Flight});
    Enter(Phase::Flight, Flight);
}

void BattlePage::Land()
{
    const GridView &At = Target();
    const bool Miss = Last.Shot.Result == Outcome::Miss;
    const std::uint32_t Life = Paced(Ctx.Plan.Speed, Miss ? MissMs : HitMs);
    Fx.Add(Effect{.Kind = Miss ? EffectKind::Splash : EffectKind::Blast,
                  .X = static_cast<float>(At.CentreX(Last.Col)),
                  .Y = static_cast<float>(At.CentreY(Last.Row)),
                  .Reach = static_cast<float>(At.Cell),
                  .Born = Ctx.Now,
                  .Life = Life,
                  .Tone = Miss ? Ctx.Chosen.Splash : Ctx.Chosen.Fire});
    Enter(Phase::Impact, Life);
}

void BattlePage::Impacted()
{
    if (Last.Shot.Result != Outcome::Sunk) {
        Settle();
        return;
    }
    const GridView &At = Target();
    const Ship &Gone = Game().Aimed().Waters.Ships()[static_cast<std::size_t>(Last.Shot.ShipIndex)];
    const std::uint32_t Life = Paced(Ctx.Plan.Speed, SinkMs);
    Fx.Add(Effect{.Kind = EffectKind::Bubbles,
                  .X = static_cast<float>(At.X + (Gone.Col + (Gone.Vertical ? 0.5f : Gone.Length * 0.5f)) * static_cast<float>(At.Cell)),
                  .Y = static_cast<float>(At.Y + (Gone.Row + (Gone.Vertical ? Gone.Length * 0.5f : 0.5f)) * static_cast<float>(At.Cell)),
                  .Reach = static_cast<float>(At.Cell),
                  .Length = static_cast<float>(Gone.Length * At.Cell) * 0.9f,
                  .Born = Ctx.Now,
                  .Life = Life});
    Enter(Phase::Sinking, Life);
}

// The shot has fallen: it is counted, and said.
void BattlePage::Settle()
{
    Before.reset();
    Said = Spoken(Last);
    const char *Word = Last.Shot.Result == Outcome::Miss ? "miss" : (Last.Shot.Result == Outcome::Hit ? "hit" : "SUNK");
    Calls.push_back(std::format("{} {}", CellName(Last.Row, Last.Col), Word));
    SaidGood = PersonShoots() ? Last.Shot.Result != Outcome::Miss : Last.Shot.Result == Outcome::Miss;
    const std::uint32_t Pause = Ctx.Plan.Speed == Pace::Instant ? QuickMs : Paced(Ctx.Plan.Speed, SettleMs);
    Enter(Phase::Settle, Pause);
}

void BattlePage::Next()
{
    const bool Quick = Ctx.Plan.Speed == Pace::Instant;
    if (Last.Won) {
        Enter(Phase::Hold, Quick ? QuickMs : Paced(Ctx.Plan.Speed, WonMs));
    } else if (Last.Again) {
        if (Game().Shooting().Computer) Enter(Phase::Thinking, Paced(Ctx.Plan.Speed, ThinkMs));
        else Enter(Phase::Aim, 0);
    } else {
        Enter(Phase::Hold, Quick ? QuickMs : Paced(Ctx.Plan.Speed, HoldMs));
    }
}

void BattlePage::Close()
{
    if (Last.Won) {
        Ctx.GameEnded();
        return;
    }
    const std::string Joined = std::accumulate(Calls.begin(), Calls.end(), std::string(), [](std::string Text, const std::string &One) {
        return Text.empty() ? One : Text + ", " + One;
    });
    Ctx.Recap = std::format("{}'s turn: {}.", Game().Shooting().Name, Joined);
    Ctx.TurnPassed();
}

std::string BattlePage::Spoken(const Report &Result) const
{
    const Player &Shooter = Game().Shooting();
    std::string   Line = std::format("{} fires at {}: ", Shooter.Name, CellName(Result.Row, Result.Col));
    if (Result.Shot.Result == Outcome::Miss) {
        Line += "miss.";
    } else if (Result.Shot.Result == Outcome::Hit) {
        Line += "HIT!";
    } else {
        const Ship &Gone = Game().Aimed().Waters.Ships()[static_cast<std::size_t>(Result.Shot.ShipIndex)];
        Line += std::format("SUNK! The {} goes down.", ShipName(Gone.Length));
    }
    if (Result.Won) return Line + " The whole fleet is destroyed!";
    if (Result.Again) return Line + (Game().Chosen().Salvo ? std::format(" {} shots left.", Game().ShotsLeft()) : " Fire again.");
    return Line + " The turn is over.";
}

void BattlePage::Move(int Down, int Across)
{
    const int Size = Game().Chosen().Grid;
    CursorRow = std::clamp(CursorRow + Down, 0, Size - 1);
    CursorCol = std::clamp(CursorCol + Across, 0, Size - 1);
}

void BattlePage::Key(const tui::KeyEvent &Pressed)
{
    if (Leaving.Open()) {
        if (Leaving.Answer(Pressed) == LeavePrompt::Verdict::Leave) Ctx.Go(PageId::Title);
        return;
    }
    if (Help) {
        Help = false;
        return;
    }
    if (Chose(Ctx.Chosen, KeyHelp, Pressed)) {
        Help = true;
    } else if (Pressed.IsExtended() && Now == Phase::Aim) {
        switch (Pressed.Scancode) {
        case tui::KeyCodes::Up:    Move(-1, 0); break;
        case tui::KeyCodes::Down:  Move(1, 0); break;
        case tui::KeyCodes::Left:  Move(0, -1); break;
        case tui::KeyCodes::Right: Move(0, 1); break;
        case tui::KeyCodes::Home:  CursorCol = 0; break;
        case tui::KeyCodes::End:   CursorCol = Game().Chosen().Grid - 1; break;
        case tui::KeyCodes::PageUp:   CursorRow = 0; break;
        case tui::KeyCodes::PageDown: CursorRow = Game().Chosen().Grid - 1; break;
        default: break;
        }
    } else if (Pressed.IsExtended() || Pressed.IsCtrl() || Pressed.IsAlt()) {
        return;
    } else if (Pressed.Ascii == tui::KeyCodes::Escape) {
        Leaving.Ask();
    } else if (Pressed.Ascii == tui::KeyCodes::Enter || Pressed.Ascii == ' ') {
        if (Now == Phase::Hold) {
            Close();
        } else if (Now == Phase::Aim) {
            if (Game().CanFire(CursorRow, CursorCol)) {
                Fire(CursorRow, CursorCol);
            } else {
                const bool Halo = Game().Aimed().Waters.MarkAt(CursorRow, CursorCol) == Mark::Halo;
                Said = Halo ? "No ship can lie there: it is next to one that has sunk." : "That cell has been fired at already.";
                SaidGood = false;
            }
        }
    }
}

std::unique_ptr<Page> MakeBattle(Context &Shared)
{
    return std::make_unique<BattlePage>(Shared);
}

}  // namespace seabattle
