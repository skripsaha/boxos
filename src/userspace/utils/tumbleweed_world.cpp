#include "tumbleweed.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "box/clock.h"
#include "box/cpu.h"

namespace {

using Color = TumbleColor;
namespace Colors = painter::core::Colors;

}

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
    return "day";
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
    : Width(std::max(Columns * 3, 120)), Height(std::max(Rows * 2 + 20, 56)), ViewWidth(Columns),
      ViewHeight((Rows - 1) * 2), Dice(cpu_rdtsc() | 1)
{
    MakeGround();
    MakeScene();
    GoalX = CameraX = static_cast<float>(std::max(0, Width - ViewWidth) / 2);
    GoalY = CameraY = static_cast<float>(std::max(0, Height - ViewHeight) / 3);
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

int TumbleWorld::Sun() const
{
    switch (Light) {
    case TumbleLight::Day: return 255;
    case TumbleLight::Dusk: return 112;
    case TumbleLight::Night: return 0;
    case TumbleLight::Clock: {
        const int Now = Hour * 60 + Minute;
        if (Now >= 7 * 60 && Now < 19 * 60) return 255;
        if (Now >= 5 * 60 + 30 && Now < 7 * 60) return (Now - 330) * 255 / 90;
        if (Now >= 19 * 60 && Now < 20 * 60 + 30) return (1230 - Now) * 255 / 90;
        return 0;
    }
    }
    return 0;
}

bool TumbleWorld::Night() const { return Sun() < 48; }

Color TumbleWorld::Shade(Color Ink, int Amount) const
{
    return Color::Mix(Colors::Black, Ink, static_cast<std::uint8_t>(std::clamp(Amount, 0, 255)));
}

Color TumbleWorld::SkyAt(int WorldY, int Sunlight) const
{
    const int Horizon = std::clamp(WorldY * 255 / std::max(1, Height * 3 / 4), 0, 255);
    if (Sunlight < 48)
        return Color::Mix(Color::Rgb(0x08, 0x16, 0x38), Color::Rgb(0x38, 0x3A, 0x72),
                          static_cast<std::uint8_t>(Horizon));
    if (Sunlight < 180)
        return Color::Mix(Color::Rgb(0x6A, 0x4A, 0x76), Color::Rgb(0xE0, 0x74, 0x51),
                          static_cast<std::uint8_t>(Horizon));
    const Color Top = Season == TumbleSeason::Winter ? Color::Rgb(0x63, 0x9A, 0xB7) : Color::Rgb(0x2B, 0x88, 0xC0);
    const Color Low = Season == TumbleSeason::Winter ? Color::Rgb(0xD1, 0xD7, 0xD0) : Color::Rgb(0xF0, 0xAE, 0x63);
    return Color::Mix(Top, Low, static_cast<std::uint8_t>(Horizon));
}

Color TumbleWorld::Sand() const
{
    switch (Season) {
    case TumbleSeason::Spring: return Color::Rgb(0xA9, 0x7A, 0x42);
    case TumbleSeason::Summer: return Color::Rgb(0xD0, 0x91, 0x43);
    case TumbleSeason::Autumn: return Color::Rgb(0xA8, 0x63, 0x36);
    case TumbleSeason::Winter: return Color::Rgb(0xB4, 0xA1, 0x83);
    }
    return Color::Rgb(0xD0, 0x91, 0x43);
}

void TumbleWorld::Step(float Seconds)
{
    Time += Seconds;
    if (Light == TumbleLight::Clock && (ClockRead < 0 || Time - ClockRead >= 1.0f)) {
        BoxTime Now;
        if (clock_boxtime_local(&Now) == 0) { Hour = Now.hour; Minute = Now.minute; }
        ClockRead = Time;
    }
    float Wind = Weather == TumbleWeather::Clear ? 0.0f : Weather == TumbleWeather::Wind ? 2.0f : 4.0f;
    if (Weather == TumbleWeather::Storm) Wind = 7.0f;
    for (TumbleWeed &One : Weeds) {
        One.X += (One.Speed + Wind) * Seconds;
        One.Spin += Seconds * (3.0f + std::abs(One.Speed));
        if (One.X < -8) One.X = static_cast<float>(Width + 6);
        if (One.X > Width + 8) One.X = -6.0f;
    }
    for (TumbleBird &One : Birds) {
        One.X += One.Speed * Seconds;
        One.Y += std::sin(Time * 2.0f + One.Phase) * Seconds * 0.8f;
        if (One.X < -12) One.X = static_cast<float>(Width + 12);
        if (One.X > Width + 12) One.X = -12.0f;
    }
    if (Weather == TumbleWeather::Dust || Weather == TumbleWeather::Storm) {
        const float Rate = Weather == TumbleWeather::Storm ? 30.0f : 14.0f;
        if (Dusts.size() < 180 && Dice.Chance(Seconds * Rate))
            Dusts.push_back({CameraX + ViewWidth + 2.0f, CameraY + Dice.Between(8, static_cast<float>(ViewHeight - 4)),
                             Dice.Between(-8.0f, -2.0f), Dice.Between(0.8f, 2.5f), Color::Rgb(0xC0, 0x97, 0x62)});
    }
    for (std::size_t At = Dusts.size(); At-- > 0;) {
        TumbleDust &One = Dusts[At];
        One.X += One.Speed * Seconds;
        One.Life -= Seconds;
        if (One.Life <= 0 || One.X < CameraX - 8) Dusts.erase(Dusts.begin() + static_cast<std::ptrdiff_t>(At));
    }
    CameraX += (GoalX - CameraX) * std::min(1.0f, Seconds * 8.0f);
    CameraY += (GoalY - CameraY) * std::min(1.0f, Seconds * 8.0f);
}

void TumbleWorld::Move(int Dx, int Dy)
{
    GoalX = std::clamp(GoalX + static_cast<float>(Dx), 0.0f, static_cast<float>(MaxX()));
    GoalY = std::clamp(GoalY + static_cast<float>(Dy), 0.0f, static_cast<float>(MaxY()));
}

void TumbleWorld::CycleWeather() { Weather = static_cast<TumbleWeather>((static_cast<int>(Weather) + 1) % 5); }
void TumbleWorld::CycleSeason() { Season = static_cast<TumbleSeason>((static_cast<int>(Season) + 1) % 4); }
void TumbleWorld::CycleLight() { Light = static_cast<TumbleLight>((static_cast<int>(Light) + 1) % 4); ClockRead = -1; }
