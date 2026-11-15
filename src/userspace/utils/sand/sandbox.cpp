/* sandbox.cpp — the brush, the keys and time turned into steps. */

#include "sandbox.h"

#include <algorithm>

#include "box/clock.h"

namespace sand {

namespace {

namespace tui = painter::tui;

// Time owed to the sand is never more than this: a stall is not made up for by a rush.
constexpr std::uint64_t MostOwedUs = 250000;
// Arrows held down speed up: a press this soon after the last goes on the same way.
constexpr std::uint64_t RunUs      = 200000;
constexpr int           LargestBrush = 16;
constexpr int           SlowestSpeed = 5;
constexpr int           FastestSpeed = 480;
// A pour fills this share of the free cells it touches, so powders and liquids
// come as a spray; solids fill whole.
constexpr int           SprayPercent = 40;
constexpr int           RainRarity   = 80;
constexpr int           StormRarity  = 100;

}  // namespace

Sandbox::Sandbox(const Scene &Begin, std::uint64_t Seed)
    : Shown(ReadLook()), Field(Seed), Brush(Shown.Brush), Speed(static_cast<int>(Shown.Speed)),
      Upward(Shown.GravityUp), Begun(Begin)
{
}

void Sandbox::Open(int Wide, int High, std::uint64_t NowUs)
{
    Field.Resize(Wide, High);
    CursorX = Wide / 2;
    CursorY = High / 3;
    Last    = NowUs;
    Tune();
    Begun.Build(Field);
}

void Sandbox::Tune()
{
    Field.Flow = Shown.Flow;
    Field.Fall = Upward ? -1 : 1;
}

void Sandbox::Reread()
{
    const Look Old = Shown;
    Shown          = ReadLook();
    if (Shown.Speed != Old.Speed) Speed = static_cast<int>(Shown.Speed);
    if (Shown.Brush != Old.Brush) Brush = Shown.Brush;
    if (Shown.GravityUp != Old.GravityUp) Upward = Shown.GravityUp;
    Tune();
}

Kind Sandbox::Material() const
{
    return Chosen == EraserAt ? Kind::Air : Pourable[static_cast<std::size_t>(Chosen)];
}

void Sandbox::Pour()
{
    const Kind What    = Material();
    const bool Whole   = What == Kind::Air || TraitOf(What).Moves == Habit::Solid;
    Field.Stamp(CursorX, CursorY, Brush, What, Whole ? 100 : SprayPercent);
}

void Sandbox::StepOnce()
{
    if (Pours) Pour();
    if (Weather == Sky::Rain) Field.Weather(Kind::Water, RainRarity);
    else if (Weather == Sky::Sandstorm) Field.Weather(Kind::Sand, StormRarity);
    Field.Step();
}

void Sandbox::Advance(std::uint64_t NowUs)
{
    const std::uint64_t Spent = NowUs - Last;
    Last                      = NowUs;
    if (Paused) {
        Owed = 0;
        return;
    }
    Owed = std::min(Owed + Spent, MostOwedUs);
    const std::uint64_t Period = 1000000u / static_cast<std::uint64_t>(Speed);
    while (Owed >= Period) {
        StepOnce();
        Owed -= Period;
    }
}

void Sandbox::Move(int Dx, int Dy, std::uint64_t NowUs)
{
    const int Way = Dx != 0 ? (Dx > 0 ? 0 : 1) : (Dy > 0 ? 2 : 3);
    Run           = Way == MovedWay && NowUs - MovedAt < RunUs ? Run + 1 : 0;
    MovedWay      = Way;
    MovedAt       = NowUs;
    const int Distance = 1 + std::min(Run / 5, 4);
    for (int Done = 0; Done < Distance; Done++) {
        const int X = std::clamp(CursorX + Dx, 0, Field.Width - 1);
        const int Y = std::clamp(CursorY + Dy, 0, Field.Height - 1);
        if (X == CursorX && Y == CursorY) break;
        CursorX = X;
        CursorY = Y;
        if (Pours) Pour();
    }
}

void Sandbox::Choose(int Index)
{
    Chosen = Index;
    Pours  = true;
}

void Sandbox::Restart()
{
    Field.Clear();
    Begun.Build(Field);
}

bool Sandbox::OnKey(const tui::KeyEvent &Key)
{
    if (Helping) {
        Helping = false;
        return true;
    }
    const kb_event_t Raw{Key.Character, Key.Scancode, Key.Mods};
    const auto       Is = [&](SandKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Helping = true;
    } else if (Is(KeyFaster)) {
        Speed = std::min(FastestSpeed, Speed + std::max(1, Speed / 4));
    } else if (Is(KeySlower)) {
        Speed = std::max(SlowestSpeed, Speed - std::max(1, Speed / 5));
    } else if (Is(KeyGravity)) {
        Upward = !Upward;
        Tune();
    }
    if (Key.IsCtrl() || Key.IsAlt()) return true;

    if (Key.IsExtended()) {
        const std::uint64_t Now = clock_uptime_us();
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:  Move(-1, 0, Now); break;
        case tui::KeyCodes::Right: Move(1, 0, Now); break;
        case tui::KeyCodes::Up:    Move(0, -1, Now); break;
        case tui::KeyCodes::Down:  Move(0, 1, Now); break;
        default:                   break;
        }
        return true;
    }
    switch (Key.Character) {
    case tui::KeyCodes::Escape:
    case 'q':
    case 'Q': return false;
    case tui::KeyCodes::Enter:
    case '\r': Pours = !Pours; break;
    case ' ':  Paused = !Paused; break;
    case tui::KeyCodes::Tab: Choose((Chosen + 1) % (EraserAt + 1)); break;
    case 'e':
    case 'E':
    case tui::KeyCodes::Backspace: Choose(EraserAt); break;
    case '+':
    case '=': Brush = std::min(LargestBrush, Brush + 1); break;
    case '-':
    case '_': Brush = std::max(1, Brush - 1); break;
    case 'w':
    case 'W': Weather = static_cast<Sky>((static_cast<int>(Weather) + 1) % 3); break;
    case 'c':
    case 'C': Field.Clear(); break;
    case 'r':
    case 'R': Restart(); break;
    default:
        if (Key.Character >= '1' && Key.Character <= '9') Choose(Key.Character - '1');
        else if (Key.Character == '0') Choose(EraserAt - 1);
        break;
    }
    return true;
}

}  // namespace sand
