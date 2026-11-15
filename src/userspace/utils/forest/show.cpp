/* show.cpp — the loop: frames by the clock, the forest's life in steady ticks
 * the clock owes, the machine's day or a chosen one, and the keys. */

#include "show.h"

#include <algorithm>
#include <cmath>

#include "box/clock.h"
#include "box/cpu.h"
#include "key_chord.h"

namespace forest {

namespace {

namespace tui = painter::tui;

// Frames come by the clock and life is stepped in ticks the clock owes, never
// more than MostOwedUs at once, so a stall does not become a leap.
constexpr std::uint64_t FrameUs     = 40000;
constexpr std::uint64_t TickUs      = 25000;
constexpr std::uint64_t MostOwedUs  = 8 * TickUs;
constexpr float         TickSeconds = static_cast<float>(TickUs) / 1e6f;
constexpr float         KeptSeconds = 4.0f;       // the line stays this long after a change
constexpr float         ClockEvery  = 5.0f;       // seconds between readings of the machine's clock
constexpr float         StepMinutes = 30.0f;
constexpr std::uint32_t MostSpeed   = 3600;

std::uint32_t Faster(std::uint32_t Speed) { return std::min(MostSpeed, Speed * 2); }
std::uint32_t Slower(std::uint32_t Speed) { return std::max(1u, Speed / 2); }

}  // namespace

Look Said(const Look &Set, const Wanted &Words, const Date &Day)
{
    Look Out = Set;
    if (Words.Of) {
        Out.SeasonByDate = false;
        Out.StartSeason  = *Words.Of;
    }
    if (Words.Is) Out.StartWeather = *Words.Is;
    if (Words.Minute) {
        Out.RealTime    = false;
        Out.StartMinute = static_cast<std::uint32_t>(*Words.Minute);
    }
    if (Words.At) {
        Season Of  = Out.SeasonByDate ? SeasonOfDate(Day) : Out.StartSeason;
        int    Doy = Out.SeasonByDate ? DayInYear(Day) : SeasonMiddle(Of);
        float  Minute = *Words.At == Part::Dawn ? SunriseMinute(Doy) + 20.0f : *Words.At == Part::Dusk ? SunsetMinute(Doy) - 20.0f
                      : *Words.At == Part::Noon ? 750.0f : 0.0f;
        Out.RealTime    = false;
        Out.StartMinute = static_cast<std::uint32_t>(std::lround(Minute));
    }
    return Out;
}

void ReadMachineMoment(Date &Day, float &Minute)
{
    BoxTime Now;
    if (clock_boxtime_local(&Now) != 0) return;
    Day    = Date{Now.year, Now.month, Now.day};
    Minute = static_cast<float>(Now.hour * 60 + Now.minute) + static_cast<float>(Now.second) / 60.0f;
}

Show::Show(tui::Stage Opened, const Look &Set, const Look &Starting, const Date &Day, float Minute)
    : Screen(std::move(Opened)), Cols(Screen.Size().Width), Rows(Screen.Size().Height), Settled(Set), Chosen(Starting),
      Picture(Cols, Rows * 2), Forest(Starting, Cols, Rows * 2, cpu_rdtsc() * 0x9E3779B97F4A7C15ull, Day, Minute),
      Real(Starting.RealTime), Speed(Starting.Speed)
{
    Screen.HearEnd();
}

// The forest's own day is the machine's, read now and then; between readings
// it goes on by itself.
void Show::Resync()
{
    Date  Day;
    float Minute = Forest.Weather.Minutes;
    ReadMachineMoment(Day, Minute);
    Forest.Weather.SetMoment(Day, Minute);
    SinceClock = 0;
}

void Show::Tick(float Seconds)
{
    if (Real) {
        Forest.Weather.Advance(Seconds / 60.0f);
        SinceClock += Seconds;
        if (SinceClock >= ClockEvery) Resync();
    } else if (!Paused) {
        Forest.Weather.Advance(Seconds * static_cast<float>(Speed) / 60.0f);
    }
    Forest.Step(Seconds);
}

void Show::Say(float Seconds)
{
    SayUntil = std::max(SayUntil, Forest.Weather.Clock + Seconds);
}

// What the settings say now, over what is running: colours, counts, keys and
// speed at once, and a start the person changed — the season, the weather, the
// clock or the minute — is taken as they changed it.
void Show::Reread()
{
    const Look Fresh = ReadLook();
    Nature    &Weather = Forest.Weather;
    Forest.Apply(Fresh);
    if (Fresh.SeasonByDate != Settled.SeasonByDate || Fresh.StartSeason != Settled.StartSeason) {
        if (Fresh.SeasonByDate) Weather.FollowDate();
        else Weather.SetSeason(Fresh.StartSeason);
    }
    if (Fresh.StartWeather != Settled.StartWeather) Weather.SetWeather(Fresh.StartWeather);
    if (Fresh.RealTime != Settled.RealTime) {
        Real = Fresh.RealTime;
        if (Real) Resync();
    }
    if (Fresh.StartMinute != Settled.StartMinute && !Real) Weather.Advance(static_cast<float>(Fresh.StartMinute) - Weather.Minutes);
    if (Fresh.Speed != Settled.Speed) Speed = Fresh.Speed;
    Chosen = Settled = Fresh;
    Say(KeptSeconds);
}

bool Show::Draw()
{
    PaintForest(Forest, Picture);
    tui::Frame &Glass = Screen.FrameBuffer();
    if (!Glass.PutPixels(Picture.All())) return false;
    if (Help || Forest.Weather.Clock < SayUntil) {
        int Minutes = static_cast<int>(Forest.Weather.Minutes);
        PaintBar(Glass, Chosen,
                 Saying{Forest.Weather.CurrentSeason(), Forest.Weather.CurrentWeather(), Minutes / 60 % 24, Minutes % 60, Real, Paused,
                        Speed, Forest.Herd.Living()});
    }
    if (!Screen.Present()) return false;
    Frames++;
    return true;
}

// False to leave.
bool Show::OnKey(const tui::KeyEvent &Key)
{
    const kb_event_t Raw{Key.Character, Key.Scancode, Key.Mods};
    const auto       Is = [&](ForestKey Which) { return KeyChordMatches(&Chosen.Keys[Which], &Raw); };
    Say(KeptSeconds);
    if (Is(KeySeason)) {
        Forest.Weather.NextSeason();
    } else if (Is(KeyWeather)) {
        Forest.Weather.NextWeather();
    } else if (Is(KeyTime)) {
        Real = !Real;
        if (Real) {
            Resync();
            if (Chosen.SeasonByDate) Forest.Weather.FollowDate();
        }
        Paused = false;
    } else if (Is(KeyPause)) {
        Paused = Real ? true : !Paused;
        Real   = false;
    } else if (Is(KeyHelp)) {
        Help = !Help;
    } else if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:  Forest.Weather.Advance(-StepMinutes); Real = false; break;
        case tui::KeyCodes::Right: Forest.Weather.Advance(StepMinutes); Real = false; break;
        case tui::KeyCodes::Up:    Speed = Faster(Speed); Real = false; break;
        case tui::KeyCodes::Down:  Speed = Slower(Speed); Real = false; break;
        default: break;
        }
    } else if (!Key.IsCtrl() && !Key.IsAlt() && (Key.Character == 'q' || Key.Character == 'Q' || Key.Character == tui::KeyCodes::Escape)) {
        return false;
    }
    return true;
}

// False if the screen stopped taking frames. The timed Stage wait lets the
// core rest and still hears a key at once.
bool Show::Run()
{
    std::uint64_t Last = clock_uptime_us();
    std::uint64_t Next = Last;
    std::uint64_t Owed = 0;

    for (;;) {
        std::uint64_t Now = clock_uptime_us();
        if (Now >= Next) {
            Owed += std::min(Now - Last, MostOwedUs);
            Last = Now;
            for (; Owed >= TickUs; Owed -= TickUs) Tick(TickSeconds);
            if (Watch.changed()) Reread();
            if (!Draw()) return false;
            Next = clock_uptime_us() + FrameUs;
            continue;
        }

        auto             LeftMs = static_cast<std::uint32_t>((Next - Now) / 1000u);
        tui::Event       Event  = Screen.Wait(LeftMs == 0 ? 1 : LeftMs);
        if (Event.Kind == tui::EventKind::Key) {
            if (!OnKey(Event.Key)) return true;
        } else if (Event.Kind == tui::EventKind::Gone || Event.Kind == tui::EventKind::EndAsked) {
            return true;
        }
    }
}

}  // namespace forest
