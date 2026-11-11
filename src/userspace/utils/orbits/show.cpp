// show.cpp — time, keys and settings of the sky.

#include "show.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "box/clock.h"

#include "presets.h"

namespace orbits {

namespace {

namespace tui = painter::tui;

constexpr std::uint64_t FrameUs      = 66667;    // a frame of fifteen in a second
constexpr std::uint64_t WorkUs       = 22000;    // the most of one frame given to stepping time
constexpr std::uint64_t PausedWaitMs = 100;      // how often a paused sky looks at its settings
constexpr std::uint64_t ForecastEveryUs = 1000000;   // how old a path may be while nothing is touched
constexpr std::uint64_t NoteUs       = 3000000;
constexpr std::uint64_t KeysShownUs  = 8000000;
constexpr int           TrailEvery   = 6;        // steps between two marks of a trail: 1/120 s of sky
constexpr double        MostPassed   = 0.1;      // longer than this since the last frame is not caught up
constexpr double        LeastSpeed   = 1.0 / 16.0;
constexpr double        MostSpeed    = 32.0;
constexpr double        LeastZoom    = 0.25;
constexpr double        MostZoom     = 6.0;
constexpr double        MostPower    = 6.0;
constexpr double        LeastPower   = 0.05;
constexpr double        MoveStep     = 0.05;
constexpr std::size_t   ForecastPlaces = 80;
constexpr int           ForecastSteps  = 4;
constexpr double        ForecastSeconds = 6.0 * StepSeconds;

constexpr double Masses[] = {0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0};
constexpr std::size_t FirstMass = 2;

constexpr Vec FirstLauncher{-1.1, 0.0};

double Degrees(double Radians)
{
    return Radians * 180.0 / std::numbers::pi;
}

double Fraction(double Value)
{
    return Value - std::floor(Value);
}

}  // namespace

Show::Show(tui::Stage Opened, std::optional<std::size_t> First)
    : Screen(std::move(Opened)), Shown(ReadLook()), Began(clock_uptime_us())
{
    Speed = std::clamp(Shown.SpeedPercent / 100.0, LeastSpeed, MostSpeed);
    Bodies.SetGravity(BaseGravity * Shown.GravityPercent / 100.0);
    Begin(First ? *First : Shown.Preset);
}

void Show::Close()
{
    (void)Screen.Close();
}

// The sky made over as a preset, trails and counts begun again.
void Show::Begin(std::size_t Which)
{
    Preset = Which;
    Prepare(Bodies, Which, clock_uptime_us());
    MassAt = FirstMass;
    Picture.Clear();
    Launcher = FirstLauncher;
    Retarget();
}

// The throw set to a circular orbit round what is at the middle, from where the
// launcher stands: the first body thrown settles at once.
void Show::Retarget()
{
    double Mass = Bodies.StarsMass();
    if (Mass <= 0.0)
        for (const Body &One : Bodies.Bodies) Mass += One.Mass;
    const double Away = std::hypot(Launcher.X, Launcher.Y);
    Power             = std::clamp(Mass > 0.0 && Away > 0.0 ? Bodies.CircularSpeed(Away, Mass) : 1.0, LeastPower, MostPower);
    Angle             = std::atan2(Launcher.X, -Launcher.Y);
}

void Show::Apply(const Look &Fresh)
{
    const Look Was = Shown;
    Shown          = Fresh;
    Bodies.SetGravity(BaseGravity * Shown.GravityPercent / 100.0);
    if (Shown.SpeedPercent != Was.SpeedPercent) Speed = std::clamp(Shown.SpeedPercent / 100.0, LeastSpeed, MostSpeed);
    if (Shown.Preset != Was.Preset) Begin(Shown.Preset);
    Stale = true;
}

// Time of Seconds, as many fixed steps as it holds; when the glass wants a
// picture before they are done, the rest of the time is let go and the sky runs
// slower than the clock, not later and later.
void Show::Simulate(double Seconds)
{
    Owed += Seconds * Speed;
    const std::uint64_t Limit = clock_uptime_us() + WorkUs;
    int                 Steps = 0;
    while (Owed >= StepSeconds) {
        Bodies.Advance(StepSeconds);
        Owed -= StepSeconds;
        if (++SinceTrail == TrailEvery) {
            SinceTrail = 0;
            for (Body &One : Bodies.Bodies) Picture.Trail(One, Shown);
        }
        if ((++Steps & 15) == 0 && clock_uptime_us() > Limit) {
            Owed = 0.0;
            break;
        }
    }
}

void Show::TakeCollisions()
{
    for (const Collision &Hit : Bodies.Collisions) Picture.Flare(Hit, Shown);
    Bodies.Collisions.clear();
}

void Show::Say(std::string Words)
{
    Note     = std::move(Words);
    NoteEnds = clock_uptime_us() + NoteUs;
}

void Show::Throw()
{
    const Vec    Way{std::cos(Angle), std::sin(Angle)};
    const double Tone = Fraction(0.37 + 0.618034 * Thrown);
    if (!Bodies.Add(Launcher, Way * Power, Masses[MassAt], Tone)) {
        Say("the sky is full");
        return;
    }
    Thrown++;
}

bool Show::OnChord(const tui::KeyEvent &Key)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](OrbitKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) Helping = true;
    else if (Is(KeyClear)) Picture.Clear();
    else if (Is(KeyReset)) Begin(Preset);
    else if (Is(KeyAim)) Shown.ShowAim = !Shown.ShowAim;
    else return false;
    return true;
}

void Show::OnExtended(const tui::KeyEvent &Key)
{
    const bool Fine = Key.IsShift();
    switch (Key.Scancode) {
    case tui::KeyCodes::Left:     Angle += Fine ? 0.5 * std::numbers::pi / 180.0 : 3.0 * std::numbers::pi / 180.0; break;
    case tui::KeyCodes::Right:    Angle -= Fine ? 0.5 * std::numbers::pi / 180.0 : 3.0 * std::numbers::pi / 180.0; break;
    case tui::KeyCodes::Up:       Power = std::min(Power + (Fine ? 0.01 : 0.05), MostPower); break;
    case tui::KeyCodes::Down:     Power = std::max(Power - (Fine ? 0.01 : 0.05), LeastPower); break;
    case tui::KeyCodes::PageUp:   Picture.Zoom(std::min(Picture.Zoomed() * 1.25, MostZoom)); break;
    case tui::KeyCodes::PageDown: Picture.Zoom(std::max(Picture.Zoomed() / 1.25, LeastZoom)); break;
    default:                      break;
    }
}

void Show::OnCharacter(char Typed)
{
    if (Typed >= 'A' && Typed <= 'Z') Typed = static_cast<char>(Typed - 'A' + 'a');
    switch (Typed) {
    case tui::KeyCodes::Enter:
    case '\r':
    case ' ': Throw(); break;
    case 'p': Paused = !Paused; break;
    case '+':
    case '=': Speed = std::min(Speed * 2.0, MostSpeed); break;
    case '-':
    case '_': Speed = std::max(Speed / 2.0, LeastSpeed); break;
    case '[': MassAt = MassAt > 0 ? MassAt - 1 : 0; break;
    case ']': MassAt = std::min(MassAt + 1, std::size(Masses) - 1); break;
    case 'w': Launcher.Y = std::min(Launcher.Y + MoveStep, 3.0); break;
    case 's': Launcher.Y = std::max(Launcher.Y - MoveStep, -3.0); break;
    case 'a': Launcher.X = std::max(Launcher.X - MoveStep, -5.0); break;
    case 'd': Launcher.X = std::min(Launcher.X + MoveStep, 5.0); break;
    default:
        if (Typed >= '1' && Typed < static_cast<char>('1' + PresetCount)) Begin(static_cast<std::size_t>(Typed - '1'));
        break;
    }
}

bool Show::OnKey(const tui::KeyEvent &Key)
{
    Stale = true;
    if (Helping) {
        Helping = false;
        return true;
    }
    if (OnChord(Key)) return true;
    if (Key.IsCtrl() || Key.IsAlt()) return true;
    if (Key.IsExtended()) {
        OnExtended(Key);
        return true;
    }
    if (Key.Ascii == tui::KeyCodes::Escape) return false;
    OnCharacter(Key.Ascii);
    return true;
}

// The aim, its path worked out again only when a key may have changed what it
// depends on, or its time is out: the sky moves on, and a path from a second
// ago is a little stale, not wrong.
const Aiming &Show::Foresee()
{
    const std::uint64_t Now = clock_uptime_us();
    Foreseen.Shown          = Shown.ShowAim && !Helping;
    if (!Shown.ShowAim || (!Stale && Now - ForeseenAt < ForecastEveryUs)) return Foreseen;
    Foreseen.Launcher = Launcher;
    Foreseen.Angle    = Angle;
    const Vec      Way{std::cos(Angle), std::sin(Angle)};
    const Forecast Ahead =
        Bodies.Foretell(Launcher, Way * Power, Masses[MassAt], ForecastPlaces, ForecastSteps, ForecastSeconds);
    Foreseen.Places = Ahead.Places;
    Foreseen.Ends   = Ahead.Ends;
    ForeseenAt      = Now;
    Stale           = false;
    return Foreseen;
}

Readout Show::Reading()
{
    const std::uint64_t Now = clock_uptime_us();
    if (!Note.empty() && Now >= NoteEnds) Note.clear();
    Readout Said;
    Said.Preset    = PresetName(Preset);
    Said.Bodies    = Bodies.Bodies.size();
    Said.Merged    = Bodies.Merged;
    Said.Escaped   = Bodies.Escaped;
    Said.Speed     = Speed;
    Said.Paused    = Paused;
    Said.Mass      = Masses[MassAt];
    Said.Power     = Power;
    Said.Degrees   = (static_cast<int>(std::lround(Degrees(Angle))) % 360 + 360) % 360;
    Said.KeysFirst = Now - Began < KeysShownUs;
    Said.Note      = Note;
    for (std::size_t Key = 0; Key < KeyCount; Key++) {
        char Spelling[KEY_CHORD_SPELLED_MAX];
        KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
        Said.Spelled[Key] = Spelling;
    }
    return Said;
}

void Show::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    if (!(Out.Size() == Fitted)) {
        Fitted = Out.Size();
        Picture.Fit(Fitted);
    }
    Picture.Paint(Out, Bodies, Shown, Foresee());
    const Readout Said = Reading();
    PaintBar(Out, Said);
    if (Helping) PaintHelp(Out, Said);
}

bool Show::Run()
{
    Screen.HearEnd();
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) Apply(ReadLook());
        const std::uint64_t Now    = clock_uptime_us();
        const double        Passed = std::min(static_cast<double>(Now - Last) / 1e6, MostPassed);
        Last                       = Now;
        if (!Paused) Simulate(Passed);
        Picture.Age(Paused ? 0.0 : Passed, Shown);
        TakeCollisions();
        Paint();
        if (!Screen.Present()) return false;

        const std::uint64_t Spent = clock_uptime_us() - Now;
        const std::uint32_t Wait  = Paused ? static_cast<std::uint32_t>(PausedWaitMs)
                                           : Spent >= FrameUs ? 1u : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1);
        const tui::Event Heard = Screen.Wait(Wait);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return true;
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key)) return true;
    }
}

}  // namespace orbits
