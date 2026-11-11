/* cauldron.cpp — the pot, its fire and its brew. */

#include "cauldron.h"

#include <algorithm>
#include <cmath>

#include "shapes.h"

namespace wizard {

namespace {

constexpr Color Iron[] = {Rgb(0x0E, 0x0F, 0x14), Rgb(0x22, 0x25, 0x2E), Rgb(0x3A, 0x40, 0x4E), Rgb(0x66, 0x6E, 0x80), Rgb(0xA4, 0xAC, 0xC0)};
constexpr Color Bark[] = {Rgb(0x20, 0x14, 0x0C), Rgb(0x48, 0x2E, 0x1A), Rgb(0x72, 0x4C, 0x2C), Rgb(0x98, 0x6C, 0x3E)};
constexpr Color Water = Rgb(0x34, 0x62, 0x52);

struct Pot {
    float Cx, Rim, Top, Ry, RimRy, BodyCy, BodyRx, BodyRy, Base;
};

Pot PotOf(const Scene &Where)
{
    Pot Out{};
    Out.Cx = static_cast<float>(Where.CauldronX);
    Out.Rim = static_cast<float>(Where.CauldronWidth) * 0.5f;
    Out.Top = static_cast<float>(Where.PotTop);
    Out.RimRy = std::max(1.8f, Out.Rim * 0.24f);
    Out.Base = static_cast<float>(Where.FeetY) - Where.Unit * 6.0f;
    Out.BodyRx = Out.Rim * 1.12f;
    Out.BodyRy = (Out.Base - Out.Top - Out.RimRy) * 0.5f;
    Out.BodyCy = Out.Top + Out.RimRy + Out.BodyRy;
    Out.Ry = Out.Rim;
    return Out;
}

// Logs crossed on the boards, glowing along their undersides.
void DrawLogs(Canvas &Out, const Scene &Where, const Pot &Of)
{
    const float Floor = static_cast<float>(Where.FeetY);
    const float Thick = std::max(1.6f, Where.Unit * 3.0f);
    const auto  Log = [&](float X0, float Y0, float X1, float Y1) {
        FillStroke(Out, X0, Y0, X1, Y1, Thick, Thick, [&](float Along, float Across, float Inside) {
            Color Here = OnRamp(Bark, 0.55f - 0.30f * Across + 0.2f * Grain(static_cast<int>(Along * 30.0f), static_cast<int>(Across * 4.0f + 8.0f), 131));
            if (Across > 0.35f) Here = Mixed(Here, Rgb(0xFF, 0x70, 0x18), 0.6f);
            if (Inside < 1.0f) Here = Darker(Here, 70);
            return Here;
        });
    };
    Log(Of.Cx - Of.Rim * 1.05f, Floor - Thick * 0.5f, Of.Cx + Of.Rim * 0.35f, Floor - Thick * 1.6f);
    Log(Of.Cx + Of.Rim * 1.05f, Floor - Thick * 0.5f, Of.Cx - Of.Rim * 0.35f, Floor - Thick * 1.6f);
}

// A tongue of the fire's colors: white-gold in its heart, orange, then red at the tip.
Color FlameColor(float Heat)
{
    static constexpr Color Tones[] = {Rgb(0x90, 0x14, 0x10), Rgb(0xE8, 0x40, 0x18), Rgb(0xFF, 0x96, 0x20), Rgb(0xFF, 0xE0, 0x60), Rgb(0xFF, 0xFA, 0xD0)};
    return OnRamp(Tones, Heat);
}

// The flames across [Left, Right] of the fire's bed, at heights that are the
// sum of a few waves and a flicker, the middle taller than the ends.
void DrawFlameColumns(Canvas &Out, const Scene &Where, const Pot &Of, const Show &Now, int Left, int Right, float Share)
{
    const float Floor = static_cast<float>(Where.FeetY) - 1.0f;
    const float Tallest = Where.Unit * 24.0f;
    for (int X = Left; X <= Right; X++) {
        const float Across = (static_cast<float>(X) - Of.Cx) / (Of.Rim * 1.15f);
        const float Tent = std::max(0.0f, 1.0f - Across * Across);
        const float Time = static_cast<float>(Now.Ms);
        const float Noise = 0.5f * Wave(static_cast<float>(X) * 0.17f + Time / 420.0f) + 0.3f * Wave(static_cast<float>(X) * 0.43f - Time / 260.0f) +
                            0.35f * (Grain(X, static_cast<int>(Now.Ms / 85), 141) - 0.5f);
        const float Height = Tallest * (0.25f + 0.75f * Tent) * Clamped(0.62f + 0.55f * Noise);
        for (int Up = 0; static_cast<float>(Up) < Height; Up++) {
            const float Heat = 1.0f - static_cast<float>(Up) / Height;
            const float Cover = Clamped(Height - static_cast<float>(Up)) * Share * Clamped(Heat * 3.0f);
            Out.Cover(X, static_cast<int>(Floor) - Up, FlameColor(Heat * (0.55f + 0.45f * Tent)), Cover);
        }
    }
}

// Rivets along the belt, the handles at the sides.
void DrawFittings(Canvas &Out, const Pot &Of)
{
    const float Y = Of.BodyCy - Of.BodyRy * 0.30f;
    for (float X = -0.85f; X <= 0.86f; X += 0.30f) {
        const float Px = Of.Cx + X * Of.BodyRx * 0.95f;
        Out.Cover(static_cast<int>(Px), static_cast<int>(Y), Iron[4], 0.9f);
        Out.Cover(static_cast<int>(Px), static_cast<int>(Y) + 1, Iron[0], 0.7f);
    }
    for (const float Side : {-1.0f, 1.0f}) {
        const float X = Of.Cx + Side * Of.BodyRx * 0.97f;
        FillStroke(Out, X, Of.Top + Of.RimRy, X + Side * Of.Rim * 0.22f, Of.Top + Of.RimRy + Of.Rim * 0.34f, 1.0f, 1.0f,
                   [](float, float, float) { return Iron[2]; });
    }
}

// The legs, in front of the fire's sides.
void DrawLegs(Canvas &Out, const Scene &Where, const Pot &Of)
{
    const float Floor = static_cast<float>(Where.FeetY);
    for (const float Side : {-1.0f, 1.0f}) {
        FillStroke(Out, Of.Cx + Side * Of.BodyRx * 0.55f, Of.Base - 1.0f, Of.Cx + Side * Of.BodyRx * 0.78f, Floor, std::max(1.4f, Of.Rim * 0.12f),
                   std::max(1.0f, Of.Rim * 0.08f), [&](float, float Across, float) { return Mixed(OnRamp(Iron, 0.45f - 0.2f * Across), Rgb(0xFF, 0x70, 0x20), 0.2f); });
    }
}

// The body: iron lit from the left above, and from the fire below in orange.
void DrawBody(Canvas &Out, const Pot &Of, const Show &Now, const Tint &Fire)
{
    FillEllipse(Out, Of.Cx, Of.BodyCy, Of.BodyRx, Of.BodyRy, [&](float Nx, float Ny, float Inside) {
        const float Key = std::max(0.0f, 1.0f - std::hypot(Nx + 0.45f, Ny + 0.45f) * 1.7f);
        float Level = 0.34f - 0.20f * Nx + 0.10f * Ny + 0.55f * Key * Key + 0.06f * Grain(static_cast<int>(Nx * 24.0f + 30.0f), static_cast<int>(Ny * 24.0f + 30.0f), 151);
        Color Here = OnRamp(Iron, Level);
        const float Glow = Clamped(Ny * 1.2f - 0.1f) * static_cast<float>(Fire.Red) / 256.0f;
        Here = Mixed(Here, Now.Fire, Clamped(Glow * 0.6f));
        if (Inside < 1.2f) Here = Darker(Here, 60);
        return Here;
    });
}

// The rim, its lip lit along the top, and the open mouth with the brew in it.
void DrawMouth(Canvas &Out, const Pot &Of, const Show &Now)
{
    FillEllipse(Out, Of.Cx, Of.Top + Of.RimRy, Of.Rim + 1.0f, Of.RimRy + 0.8f, [&](float Nx, float Ny, float Inside) {
        Color Here = OnRamp(Iron, 0.5f - 0.25f * Nx - 0.2f * Ny);
        if (Ny < -0.2f) Here = OnRamp(Iron, 0.85f);
        if (Inside < 1.0f) Here = Darker(Here, 60);
        return Here;
    });
    const Color Brew = Now.Brew == 0 ? Water : Color::FromRaw(Now.Brew);
    const Color Base = Mixed(Water, Brew, 0.20f + 0.20f * static_cast<float>(Now.Fill));
    FillEllipse(Out, Of.Cx, Of.Top + Of.RimRy + 0.3f, Of.Rim - 1.1f, std::max(1.0f, Of.RimRy - 1.0f), [&](float Nx, float Ny, float) {
        const float Ring = Wave(std::sqrt(Nx * Nx + Ny * Ny * 2.0f) * 2.2f - static_cast<float>(Now.Ms % 2400u) / 2400.0f);
        Color Here = Toned(Base, 0.80f + 0.18f * Ring + 0.25f * Grain(static_cast<int>(Nx * 12.0f + 30.0f), static_cast<int>(Ny * 12.0f + 30.0f), 161 + static_cast<std::uint32_t>(Now.Ms / 220)));
        if (Ny < -0.55f) Here = Toned(Here, 0.65f);
        return Here;
    });
}

// Bubbles that swell and burst on the surface, and the ring a thing leaves going in.
void DrawBoil(Canvas &Out, const Pot &Of, const Show &Now)
{
    const Color Brew = Now.Brew == 0 ? Water : Color::FromRaw(Now.Brew);
    const int Count = 3 + Now.Fill * 2;
    for (int Index = 0; Index < Count; Index++) {
        const std::uint32_t Roll = Hash(Index, static_cast<int>(Now.Ms / 900u + static_cast<std::uint64_t>(Index) * 3u), 171);
        const float Phase = static_cast<float>((Now.Ms + Roll % 900u) % 900u) / 900.0f;
        const float Size = Wave(Phase * 0.5f) * (1.0f + 0.6f * static_cast<float>(Roll >> 12 & 3u) / 3.0f);
        const float X = Of.Cx + (static_cast<float>(Roll & 255u) / 255.0f - 0.5f) * (Of.Rim - 3.0f) * 1.5f;
        const float Y = Of.Top + Of.RimRy + 0.3f + (static_cast<float>(Roll >> 8 & 255u) / 255.0f - 0.5f) * (Of.RimRy - 1.5f);
        if (Size < 0.2f) continue;
        FillEllipse(Out, X, Y - Size * 0.4f, Size, Size * 0.8f, [&](float Nx, float Ny, float Inside) {
            Color Here = Lighter(Brew, 70);
            if (Nx < -0.2f && Ny < -0.2f) Here = Lighter(Brew, 190);
            return Inside < 1.0f ? Lighter(Brew, 30) : Here;
        });
    }
    if (Now.Splash > 0.02f) {
        const float Reach = (1.0f - Now.Splash) * (Of.Rim - 2.0f);
        for (int Dot = 0; Dot < 36; Dot++) {
            const float Angle = static_cast<float>(Dot) / 36.0f;
            Out.Cover(static_cast<int>(Of.Cx + Reach * Wave(Angle + 0.25f)), static_cast<int>(Of.Top + Of.RimRy + 0.3f + Reach * 0.28f * Wave(Angle)),
                      Lighter(Brew, 160), Now.Splash);
        }
    }
}

}  // namespace

Spot SurfaceOf(const Scene &Where)
{
    const Pot Of = PotOf(Where);
    return Spot{Of.Cx, Of.Top + Of.RimRy};
}

Spot FireOf(const Scene &Where)
{
    const Pot Of = PotOf(Where);
    return Spot{Of.Cx, static_cast<float>(Where.FeetY) - Where.Unit * 6.0f};
}

void DrawCauldron(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights)
{
    const Pot Of = PotOf(Where);
    const Tint Fire = Lights.At(static_cast<int>(Of.Cx), static_cast<int>(Of.BodyCy));
    DrawLogs(Out, Where, Of);
    const int Mid = static_cast<int>(Of.Cx);
    const int Reach = static_cast<int>(Of.Rim * 1.1f);
    DrawFlameColumns(Out, Where, Of, Now, Mid - Reach, Mid + Reach, 0.95f);
    DrawLegs(Out, Where, Of);
    DrawBody(Out, Of, Now, Fire);
    DrawFittings(Out, Of);
    DrawFlameColumns(Out, Where, Of, Now, Mid - Reach - 1, Mid - static_cast<int>(Of.Rim * 0.85f), 0.85f);
    DrawFlameColumns(Out, Where, Of, Now, Mid + static_cast<int>(Of.Rim * 0.85f), Mid + Reach + 1, 0.85f);
    DrawMouth(Out, Of, Now);
    DrawBoil(Out, Of, Now);
    const Color Brew = Now.Brew == 0 ? Water : Color::FromRaw(Now.Brew);
    Out.Glow(Of.Cx, Of.Top - Of.Rim * 0.2f, Of.Rim * 1.4f, Brew, 25 + Now.Fill * 22 + static_cast<int>(Now.Splash * 60.0f));
}

}  // namespace wizard
