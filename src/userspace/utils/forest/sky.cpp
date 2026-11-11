/* sky.cpp — the sun, the moon and the colours between them. */

#include "sky.h"

#include <cmath>

namespace forest {

namespace {

constexpr float Pi         = 3.14159265358979323846f;
constexpr float NoonMinute = 12.5f * 60.0f;

const Color NightTop     = Color::Rgb(0x0F, 0x11, 0x16);
const Color NightHorizon = Color::Rgb(0x1B, 0x1F, 0x26);
const Color DayHaze      = Color::Rgb(0xE4, 0xF2, 0xFA);
const Color MoonGlow     = Color::Rgb(0x3E, 0x44, 0x4C);
const Color TwilightLow  = Color::Rgb(0xFF, 0x9A, 0x5A);
const Color TwilightHigh = Color::Rgb(0x7B, 0x5C, 0x8A);
const Color Overcast     = Color::Rgb(0x98, 0xA0, 0xA8);

constexpr Tint NightLight{104, 116, 136};
constexpr Tint DayLight{256, 252, 240};
constexpr Tint TwilightLight{256, 190, 150};

int Percent(float Value)
{
    return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f));
}

Tint Between(const Tint &From, const Tint &To, float Weight)
{
    auto Mix = [Weight](int A, int B) { return A + static_cast<int>(std::lround(static_cast<float>(B - A) * Weight)); };
    return {Mix(From.Red, To.Red), Mix(From.Green, To.Green), Mix(From.Blue, To.Blue)};
}

// The sun's height over the horizon, -1..1, and how far across it is.
struct SunWay {
    float Altitude;
    float Across;
    bool  Up;
    float Peak;
};

// Minutes from noon to sunset: 4 hours in midwinter, 8.2 in midsummer.
float HalfDay(int DayOfYear)
{
    float Season = std::sin(2.0f * Pi * (static_cast<float>(DayOfYear) - 80.0f) / 365.0f);
    return (6.1f + 2.1f * Season) * 60.0f;
}

SunWay SunAt(float Minutes, int DayOfYear)
{
    float Half   = HalfDay(DayOfYear);
    float Peak   = 0.45f + 0.40f * std::clamp((Half / 60.0f - 4.0f) / 4.2f, 0.0f, 1.0f);
    float Rise   = NoonMinute - Half;
    float Set    = NoonMinute + Half;
    if (Minutes >= Rise && Minutes <= Set) {
        float Along = (Minutes - Rise) / (Set - Rise);
        return {std::sin(Pi * Along) * Peak, Along, true, Peak};
    }
    float Night = 1440.0f - (Set - Rise);
    float Since = Minutes > Set ? Minutes - Set : Minutes + 1440.0f - Set;
    return {-std::sin(Pi * Since / Night) * 0.55f, 0.5f, false, Peak};
}

}  // namespace

float SunriseMinute(int DayOfYear) { return NoonMinute - HalfDay(DayOfYear); }
float SunsetMinute(int DayOfYear) { return NoonMinute + HalfDay(DayOfYear); }

SkyNow ReadSky(const Nature &Weather, const Look &Shown)
{
    SkyNow Sky;
    const SunWay Sun = SunAt(Weather.Minutes, Weather.DayOfYear());
    Sky.Day          = Smooth(-0.10f, 0.22f, Sun.Altitude);
    float Near       = 1.0f - std::min(1.0f, std::fabs(Sun.Altitude - 0.04f) / 0.28f);
    float Gloom      = Weather.Aloft.Gloom;
    Sky.Glow         = Near * Near * (1.0f - 0.6f * Gloom);
    Sky.SunUp        = Sun.Up;
    Sky.SunX         = 0.08f + 0.84f * Sun.Across;
    Sky.SunHeight    = Sun.Altitude / Sun.Peak * std::min(1.0f, Sun.Peak * 1.25f);

    Sky.MoonRound   = Weather.MoonRound();
    float Transit   = std::fmod(NoonMinute + Sky.MoonRound * 1440.0f, 1440.0f);
    float Turn      = 2.0f * Pi * (Weather.Minutes - Transit) / 1440.0f;
    float Rising    = std::cos(Turn);
    Sky.MoonUp      = Rising > 0.04f;
    Sky.MoonX       = 0.08f + 0.84f * (0.5f + 0.5f * std::sin(Turn));
    Sky.MoonHeight  = Rising * 0.8f;
    Sky.MoonLight   = (1.0f - std::cos(2.0f * Pi * Sky.MoonRound)) * 0.5f;
    float Moonlit   = Sky.MoonUp ? Sky.MoonLight * (1.0f - Sky.Day) : 0.0f;

    Color Haze = Mixed(Shown.Sky, DayHaze, 150);
    Color Top  = Mixed(NightTop, Shown.Sky, Percent(Sky.Day));
    Color Low  = Mixed(NightHorizon, Haze, Percent(Sky.Day));
    Low        = Mixed(Low, MoonGlow, Percent(Moonlit * 0.25f));
    Low        = Mixed(Low, TwilightLow, Percent(Sky.Glow * 0.8f));
    Top        = Mixed(Top, TwilightHigh, Percent(Sky.Glow * 0.28f));

    Color Grey = Mixed(Color::Rgb(0, 0, 0), Overcast, Percent(0.2f + 0.8f * Sky.Day));
    Sky.Top     = Mixed(Top, Grey, Percent(Gloom * 0.85f));
    Sky.Horizon = Mixed(Low, Grey, Percent(Gloom * 0.8f));

    Tint Light = Between(NightLight, DayLight, Sky.Day);
    Light.Red += static_cast<int>(Moonlit * 18.0f);
    Light.Green += static_cast<int>(Moonlit * 20.0f);
    Light.Blue += static_cast<int>(Moonlit * 20.0f);
    Light = Between(Light, TwilightLight, Sky.Glow * 0.55f);
    int Average = (Light.Red + Light.Green + Light.Blue) / 3;
    Light       = Between(Light, {Average, Average, Average}, Gloom * 0.5f);
    float Dim   = 1.0f - Gloom * (0.3f + 0.2f * Sky.Day);
    Sky.Light   = {static_cast<int>(static_cast<float>(Light.Red) * Dim), static_cast<int>(static_cast<float>(Light.Green) * Dim),
                   static_cast<int>(static_cast<float>(Light.Blue) * Dim)};
    return Sky;
}

}  // namespace forest
