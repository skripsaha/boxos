/* tower_life.cpp — what moves in the tower of itself: the thing in flight, the
 * spell taking hold, his hands, his gaze and his blinks, the steam and the
 * sparks. */

#include "tower.h"

#include <algorithm>

#include "cauldron.h"
#include "icons.h"

namespace wizard {

namespace {

constexpr std::uint64_t FlightMs = 650;

// Moves Value towards Target at Rate a millisecond, without overshooting.
float Eased(float Value, float Target, float Rate, std::uint32_t ElapsedMs)
{
    const float Step = std::min(1.0f, Rate * static_cast<float>(ElapsedMs));
    return Value + (Target - Value) * Step;
}

}  // namespace

void Tower::Tell(std::string Title, std::string Heard)
{
    Said.Say(std::move(Title), std::move(Heard), LastMs);
}

void Tower::Burst(float X, float Y, Color Ink, std::uint32_t Count, SpeckKind Kind)
{
    for (std::uint32_t One = 0; One < Count; One++) {
        const float Around = static_cast<float>(Luck.Below(1000)) / 500.0f - 1.0f;
        const float Lift = static_cast<float>(Luck.Below(1000)) / 1000.0f;
        const float Reach = Kind == SpeckKind::Drop ? 26.0f : 34.0f;
        const float Rise = Kind == SpeckKind::Drop ? 42.0f + 40.0f * Lift : 8.0f + 30.0f * Lift;
        Dust.Add(Speck{X, Y, Around * Reach * Where.Unit * 1.4f, -Rise * Where.Unit * 1.4f, Kind == SpeckKind::Drop ? 190.0f * Where.Unit : 14.0f,
                       600u + Luck.Below(700), 0, Ink, Kind});
    }
}

Show Tower::ShowNow() const
{
    Show Out;
    Out.Ms = LastMs;
    Out.Brew = Liquid(Brewing);
    Out.Fill = static_cast<int>(Brewing.size());
    Out.Excite = Excite;
    Out.Splash = Splash;
    Out.Glow = Chosen.Glow;
    Out.Fire = Chosen.Fire;
    Out.Candle = Chosen.Candle;
    Out.Moon = Chosen.Moon;
    Out.Critters = Chosen.Critters;
    return Out;
}

Pose Tower::Posed() const
{
    Pose Out;
    Out.Look = GazeX;
    Out.LookUp = GazeY;
    Out.Speak = Said.Mouth(LastMs, Chosen.TalkPerSecond);
    const std::uint64_t Since = LastMs - BlinkMs;
    Out.Blink = Since < 170 ? 1.0f - std::abs(static_cast<float>(Since) - 85.0f) / 85.0f : 0.0f;
    Out.Face = Face;
    Out.FaceWeight = MoodWeight;
    Out.Stir = StirLevel;
    Out.Cast = CastLevel;
    Out.Drink = DrinkLevel;
    Out.Spin = Spin;
    Out.Breath = Beat(LastMs, 3800);
    return Out;
}

std::uint32_t Tower::LeftSeconds() const
{
    return LaidUntilMs > LastMs ? static_cast<std::uint32_t>((LaidUntilMs - LastMs + 999) / 1000) : 0;
}

// The thing has reached the pot: it is in the brew, the liquid takes its color, and he stirs.
void Tower::Land()
{
    const std::size_t Which = Falling->Which;
    Falling.reset();
    Brewing.push_back(static_cast<std::uint8_t>(Which));
    const Spot Surface = SurfaceOf(Where);
    Burst(Surface.X, Surface.Y, Color::FromRaw(Ingredients()[Which].Color), 12, SpeckKind::Drop);
    Splash = 1.0f;
    StirUntilMs = LastMs + 2800;
    Tell(std::string(Ingredients()[Which].Name), Remark(Which, Brewing, Luck));
}

void Tower::Step(std::uint32_t ElapsedMs)
{
    const std::uint64_t Now = LastMs;
    if (Soon && Now >= Soon->At) {
        Pending Ready = std::move(*Soon);
        Soon.reset();
        if (Ready.Drinking) Brewing.clear();
        Lay(std::move(Ready.What));
    }
    if (Falling && Now - Falling->Began >= FlightMs) Land();
    if (LaidUntilMs != 0 && Now >= LaidUntilMs) {
        LaidUntilMs = 0;
        Tell("The spell wears off", "Everything is as it was. Mostly.");
    }

    Excite = std::max(0.0f, Excite - static_cast<float>(ElapsedMs) / 2200.0f);
    Splash = std::max(0.0f, Splash - static_cast<float>(ElapsedMs) / 750.0f);
    const bool Lying = LaidUntilMs != 0;
    MoodWeight = std::max(Lying ? 0.45f : 0.0f, MoodWeight - static_cast<float>(ElapsedMs) / (Lying ? 7000.0f : 2500.0f));
    StirLevel = Eased(StirLevel, Now < StirUntilMs ? 1.0f : 0.0f, 0.006f, ElapsedMs);
    CastLevel = Eased(CastLevel, Now < CastUntilMs ? 1.0f : 0.0f, 0.007f, ElapsedMs);
    DrinkLevel = Eased(DrinkLevel, Now < DrinkUntilMs ? 1.0f : 0.0f, 0.007f, ElapsedMs);
    if (StirLevel > 0.05f || (MoodWeight > 0.1f && Face == Mood::Dizzy)) Spin += static_cast<float>(ElapsedMs) / 1100.0f;

    // His eyes: they go where he is looking now and then wander; he blinks every few seconds.
    if (Now >= GazeUntilMs) {
        GazeToward = static_cast<float>(Luck.Below(7)) / 10.0f - 0.3f;
        GazeTowardUp = static_cast<float>(Luck.Below(5)) / 10.0f - 0.2f;
        GazeUntilMs = Now + 1800 + Luck.Below(2600);
    }
    GazeX = Eased(GazeX, GazeToward, 0.010f, ElapsedMs);
    GazeY = Eased(GazeY, GazeTowardUp, 0.010f, ElapsedMs);
    if (Now >= NextBlinkMs) {
        BlinkMs = Now;
        NextBlinkMs = Now + 2200 + Luck.Below(3600);
    }

    Dust.Step(ElapsedMs);
    SparkMs += ElapsedMs;
    const Spot Surface = SurfaceOf(Where), Fire = FireOf(Where);
    const float Rim = static_cast<float>(Where.CauldronWidth) * 0.5f;
    while (SparkMs >= 90) {
        SparkMs -= 90;
        const float Across = static_cast<float>(Luck.Below(1000)) / 500.0f - 1.0f;
        Dust.Add(Speck{Fire.X + Across * Rim, Fire.Y - Where.Unit * 4.0f, Across * 6.0f, -(10.0f + static_cast<float>(Luck.Below(24))) * Where.Unit,
                       -4.0f, 700u + Luck.Below(900), 0, Color::Rgb(0xFF, 0x9A, 0x30), SpeckKind::Spark});
        if (Luck.Below(2) == 0)
            Dust.Add(Speck{Surface.X + Across * Rim * 0.6f, Surface.Y - 1.0f, Across * 2.0f, -(5.0f + static_cast<float>(Luck.Below(7))) * Where.Unit,
                           -2.0f, 1400u + Luck.Below(1200), 0, Brewing.empty() ? Color::Rgb(0xC8, 0xD4, 0xD0) : Lighter(Color::FromRaw(Liquid(Brewing)), 150), SpeckKind::Steam});
        if (Luck.Below(4) == 0)
            Dust.Add(Speck{WizardPixelX(Where, -38.0f), WizardPixelY(Where, 6.0f), static_cast<float>(Luck.Below(21)) - 10.0f, -(4.0f + static_cast<float>(Luck.Below(10))) * Where.Unit,
                           2.0f, 900u + Luck.Below(500), 0, Chosen.Glow, SpeckKind::Spark});
        if (Falling) {
            const float T = static_cast<float>(Now - Falling->Began) / static_cast<float>(FlightMs);
            Dust.Add(Speck{Falling->FromX + (Surface.X - Falling->FromX) * T, Falling->FromY + (Surface.Y - Falling->FromY) * T - 8.0f * Where.Unit * Wave(T * 0.5f), 0.0f, 4.0f, 0.0f,
                           350u, 0, Color::FromRaw(Ingredients()[Falling->Which].Color), SpeckKind::Spark});
        }
    }
}

}  // namespace wizard
