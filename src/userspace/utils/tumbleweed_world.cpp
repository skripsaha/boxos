#include "tumbleweed.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "box/clock.h"
#include "box/cpu.h"

namespace {
namespace Colors = painter::core::Colors;
constexpr float Pi = 3.14159265358979323846f;

int SeasonSunScale(TumbleSeason Season)
{
    switch (Season) {
    case TumbleSeason::Spring: return 224;
    case TumbleSeason::Summer: return 255;
    case TumbleSeason::Autumn: return 214;
    case TumbleSeason::Winter: return 166;
    }
    return 255;
}

int SeasonWarmth(TumbleSeason Season)
{
    switch (Season) {
    case TumbleSeason::Spring: return 202;
    case TumbleSeason::Summer: return 238;
    case TumbleSeason::Autumn: return 224;
    case TumbleSeason::Winter: return 142;
    }
    return 220;
}

}  // namespace

const char *TumbleWeatherName(TumbleWeather Value)
{
    switch (Value) {
    case TumbleWeather::Clear: return "clear";
    case TumbleWeather::Wind: return "wind";
    case TumbleWeather::Dust: return "dust";
    case TumbleWeather::Rain: return "rain";
    case TumbleWeather::Storm: return "storm";
    }
    return "clear";
}

const char *TumbleSeasonName(TumbleSeason Value)
{
    switch (Value) {
    case TumbleSeason::Spring: return "spring";
    case TumbleSeason::Summer: return "summer";
    case TumbleSeason::Autumn: return "autumn";
    case TumbleSeason::Winter: return "winter";
    }
    return "summer";
}

const char *TumbleLightName(TumbleLight Value)
{
    switch (Value) {
    case TumbleLight::Clock: return "clock";
    case TumbleLight::Day: return "day";
    case TumbleLight::Dusk: return "dusk";
    case TumbleLight::Night: return "night";
    }
    return "clock";
}

TumbleDice::TumbleDice(std::uint64_t Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}

std::uint64_t TumbleDice::Next()
{
    State ^= State << 13;
    State ^= State >> 7;
    State ^= State << 17;
    return State;
}

float TumbleDice::Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
float TumbleDice::Between(float Low, float High) { return Low + (High - Low) * Unit(); }

int TumbleDice::Count(int Low, int High)
{
    return Low + static_cast<int>(Next() % static_cast<std::uint64_t>(High - Low + 1));
}

bool TumbleDice::Chance(float Probability) { return Unit() < Probability; }

TumbleWorld::TumbleWorld(int Columns, int Rows)
    : Width(std::max(Columns * 4, 160)), Height(std::max(Rows * 2 + 28, 64)), ViewWidth(Columns),
      ViewHeight((Rows - 1) * 2), Dice(cpu_rdtsc() | 1)
{
    MakeGround();
    MakeScene();
    GoalX = CameraX = static_cast<float>(std::max(0, Width - ViewWidth) / 2);
    GoalY = CameraY = static_cast<float>(std::max(0, Height - ViewHeight) / 3);
    Spawned = 0;
}

int TumbleWorld::Ground(int X) const
{
    if (Floor.empty()) return Height - 6;
    return Floor[static_cast<std::size_t>(std::clamp(X, 0, Width - 1))];
}

int TumbleWorld::DistantGround(int X) const
{
    if (Mesa.empty()) return Height / 2;
    return Mesa[static_cast<std::size_t>(std::clamp(X, 0, Width - 1))];
}

int TumbleWorld::MaxX() const { return std::max(0, Width - ViewWidth); }
int TumbleWorld::MaxY() const { return std::max(0, Height - ViewHeight); }

int TumbleWorld::DayMinutes() const
{
    switch (Light) {
    case TumbleLight::Clock: return Hour * 60 + Minute;
    case TumbleLight::Day: return 12 * 60;
    case TumbleLight::Dusk: return 19 * 60;
    case TumbleLight::Night: return 0;
    }
    return 12 * 60;
}

TumbleSky TumbleWorld::Sky() const
{
    const int Minutes = DayMinutes();
    const float Phase = std::clamp((static_cast<float>(Minutes) - 330.0f) / 900.0f, 0.0f, 1.0f);
    const float Altitude = std::sin(Phase * Pi);
    const int Scale = SeasonSunScale(Season);

    TumbleSky State;
    State.Sunlight = static_cast<int>(Altitude * static_cast<float>(Scale) + 0.5f);
    if (Light == TumbleLight::Night) State.Sunlight = 0;
    State.Night = State.Sunlight < 36;

    const int BaseWarmth = SeasonWarmth(Season);
    State.Warmth = std::clamp(BaseWarmth + (State.Sunlight < 150 ? 22 : 0) - (State.Night ? 70 : 0), 72, 255);

    State.SunX = static_cast<int>(Width * (0.12f + Phase * 0.76f));
    State.SunY = 5 + static_cast<int>((1.0f - Altitude) * Height * 0.34f);

    const float MoonPhase = std::fmod(static_cast<float>(Minutes) / 1440.0f + 0.22f, 1.0f);
    State.MoonX = static_cast<int>(Width * (0.10f + MoonPhase * 0.80f));
    State.MoonY = 9 + static_cast<int>(std::sin(MoonPhase * Pi) * 7.0f);
    return State;
}

int TumbleWorld::Sun() const { return Sky().Sunlight; }
bool TumbleWorld::Night() const { return Sky().Night; }

TumbleColor TumbleWorld::Shade(TumbleColor Ink, int Amount) const
{
    return TumbleColor::Mix(Colors::Black, Ink, static_cast<std::uint8_t>(std::clamp(Amount, 0, 255)));
}

TumbleColor TumbleWorld::Illuminate(TumbleColor Ink, int WorldY, int Layer) const
{
    const TumbleSky State = Sky();
    const int HeightLight = std::clamp(255 - WorldY * 2, 0, 96);
    const int Ambient = State.Night ? 64 : 104;
    const int SunLight = State.Sunlight * (Layer == 0 ? 3 : 4) / 5;
    const int Amount = std::clamp(Ambient + SunLight / 2 + HeightLight - (Layer == 0 ? 14 : 0), 42, 255);
    TumbleColor Out = Shade(Ink, Amount);
    if (State.Warmth < 150) {
        const TumbleColor Cool = Season == TumbleSeason::Winter ? TumbleColor::Rgb(0x70, 0x94, 0xBA)
                                                           : TumbleColor::Rgb(0x68, 0x78, 0xB0);
        Out = TumbleColor::Mix(Out, Cool, static_cast<std::uint8_t>((150 - State.Warmth) * 3 / 5));
    } else if (State.Warmth > 205 && State.Sunlight < 210) {
        Out = TumbleColor::Mix(Out, TumbleColor::Rgb(0xE0, 0x91, 0x5B), static_cast<std::uint8_t>((State.Warmth - 205) * 2));
    }
    return Out;
}

TumbleColor TumbleWorld::SkyAt(int WorldY, const TumbleSky &State) const
{
    const int Span = std::max(1, Height * 62 / 100);
    const std::uint8_t Gradient = static_cast<std::uint8_t>(std::clamp((WorldY - 2) * 255 / Span, 0, 255));
    TumbleColor Top;
    TumbleColor Horizon;

    if (State.Night) {
        Top = Season == TumbleSeason::Winter ? TumbleColor::Rgb(0x10, 0x1D, 0x36) : TumbleColor::Rgb(0x08, 0x16, 0x32);
        Horizon = Season == TumbleSeason::Winter ? TumbleColor::Rgb(0x42, 0x4D, 0x72) : TumbleColor::Rgb(0x2C, 0x35, 0x68);
    } else if (State.Sunlight < 150) {
        Top = Season == TumbleSeason::Winter ? TumbleColor::Rgb(0x43, 0x4E, 0x72) : TumbleColor::Rgb(0x51, 0x3B, 0x68);
        Horizon = Season == TumbleSeason::Winter ? TumbleColor::Rgb(0xB4, 0x8B, 0x88) : TumbleColor::Rgb(0xE0, 0x78, 0x54);
    } else {
        switch (Season) {
        case TumbleSeason::Spring:
            Top = TumbleColor::Rgb(0x3A, 0x8E, 0xB0); Horizon = TumbleColor::Rgb(0xF0, 0xBD, 0x7B); break;
        case TumbleSeason::Summer:
            Top = TumbleColor::Rgb(0x20, 0x76, 0xB2); Horizon = TumbleColor::Rgb(0xF4, 0xB1, 0x68); break;
        case TumbleSeason::Autumn:
            Top = TumbleColor::Rgb(0x47, 0x70, 0xA4); Horizon = TumbleColor::Rgb(0xE4, 0x9D, 0x65); break;
        case TumbleSeason::Winter:
            Top = TumbleColor::Rgb(0x59, 0x80, 0x9C); Horizon = TumbleColor::Rgb(0xB9, 0xC8, 0xC9); break;
        }
    }
    return TumbleColor::Mix(Top, Horizon, Gradient);
}

TumbleColor TumbleWorld::Sand() const
{
    switch (Season) {
    case TumbleSeason::Spring: return TumbleColor::Rgb(0xA9, 0x79, 0x43);
    case TumbleSeason::Summer: return TumbleColor::Rgb(0xC9, 0x8A, 0x43);
    case TumbleSeason::Autumn: return TumbleColor::Rgb(0x9F, 0x5F, 0x36);
    case TumbleSeason::Winter: return TumbleColor::Rgb(0xA9, 0x9A, 0x82);
    }
    return TumbleColor::Rgb(0xC9, 0x8A, 0x43);
}

void TumbleWorld::Step(float Seconds)
{
    Time += Seconds;
    if (Light == TumbleLight::Clock && (ClockRead < 0 || Time - ClockRead >= 1.0f)) {
        BoxTime Now;
        if (clock_boxtime_local(&Now) == 0) {
            Hour = Now.hour;
            Minute = Now.minute;
        }
        ClockRead = Time;
    }

    const float Wind = Weather == TumbleWeather::Clear ? 0.0f
                     : Weather == TumbleWeather::Wind ? 2.0f
                     : Weather == TumbleWeather::Storm ? 7.0f : 4.0f;
    for (TumbleWeed &One : Weeds) {
        One.X += (One.Speed + Wind) * Seconds;
        One.Spin += Seconds * (3.0f + std::abs(One.Speed));
        if (One.X < -10) One.X = static_cast<float>(Width + 8);
        if (One.X > Width + 10) One.X = -8.0f;
    }
    for (TumbleBird &One : Birds) {
        One.X += One.Speed * Seconds;
        One.Y += std::sin(Time * 2.0f + One.Phase) * Seconds * 0.5f;
        One.Y = std::clamp(One.Y, 5.0f, static_cast<float>(Height * 0.48f));
        if (One.X < -14) One.X = static_cast<float>(Width + 12);
        if (One.X > Width + 14) One.X = -12.0f;
    }

    if (Weather == TumbleWeather::Dust || Weather == TumbleWeather::Storm) {
        const float Rate = Weather == TumbleWeather::Storm ? 30.0f : 14.0f;
        if (Dusts.size() < 240 && Dice.Chance(Seconds * Rate))
            Dusts.push_back({CameraX + ViewWidth + 2.0f,
                             CameraY + Dice.Between(8.0f, static_cast<float>(ViewHeight - 4)),
                             Dice.Between(-8.0f, -2.0f), Dice.Between(0.8f, 2.5f),
                             TumbleColor::Rgb(0xC0, 0x97, 0x62)});
    }
    for (std::size_t At = Dusts.size(); At-- > 0;) {
        TumbleDust &One = Dusts[At];
        One.X += One.Speed * Seconds;
        One.Life -= Seconds;
        if (One.Life <= 0 || One.X < CameraX - 10)
            Dusts.erase(Dusts.begin() + static_cast<std::ptrdiff_t>(At));
    }

    CameraX += (GoalX - CameraX) * std::min(1.0f, Seconds * 7.0f);
    CameraY += (GoalY - CameraY) * std::min(1.0f, Seconds * 7.0f);
}

void TumbleWorld::Move(int Dx, int Dy)
{
    GoalX = std::clamp(GoalX + static_cast<float>(Dx), 0.0f, static_cast<float>(MaxX()));
    GoalY = std::clamp(GoalY + static_cast<float>(Dy), 0.0f, static_cast<float>(MaxY()));
}

void TumbleWorld::CycleWeather()
{
    Weather = static_cast<TumbleWeather>((static_cast<int>(Weather) + 1) % 5);
}

void TumbleWorld::CycleSeason()
{
    Season = static_cast<TumbleSeason>((static_cast<int>(Season) + 1) % 4);
}

void TumbleWorld::CycleLight()
{
    Light = static_cast<TumbleLight>((static_cast<int>(Light) + 1) % 4);
    ClockRead = -1;
}

void TumbleWorld::AddWeed(float X)
{
    if (Weeds.size() >= 120) return;
    const int Layer = Dice.Chance(0.24f) ? 0 : 1;
    const float Direction = Dice.Chance(0.5f) ? 1.0f : -1.0f;
    Weeds.push_back({std::clamp(X, 2.0f, static_cast<float>(Width - 3)),
                     Direction * Dice.Between(2.5f, 8.0f), Dice.Between(0, 6.28f), Layer,
                     TumbleColor::Rgb(0x9B, 0x72, 0x40)});
    Spawned++;
}

void TumbleWorld::AddCactus(float X)
{
    if (Cacti.size() >= 120) return;
    Cacti.push_back({std::clamp(static_cast<int>(X), 4, Width - 5), Dice.Count(14, 29), Dice.Chance(0.72f), 1,
                     Dice.Chance(0.5f) ? TumbleColor::Rgb(0x4D, 0x9A, 0x4D) : TumbleColor::Rgb(0x78, 0xA6, 0x45)});
    Spawned++;
}

void TumbleWorld::AddBird(float X, float Y)
{
    if (Birds.size() >= 90) return;
    const float Speed = Dice.Between(4.0f, 10.0f) * (Dice.Chance(0.5f) ? 1.0f : -1.0f);
    Birds.push_back({X, Y, Speed, Dice.Between(0, 6.28f), Dice.Chance(0.45f) ? 0 : 1, Speed < 0,
                     TumbleColor::Rgb(0xC0, 0xB8, 0xA0)});
    Spawned++;
}
