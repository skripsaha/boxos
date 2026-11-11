/* figure_build.cpp — the wizard's body, drawn once: shoes, robe with its pleats,
 * belt and trim in one sprite; face, beard, moustache, brim, hat and its stars
 * in another. Everything is described in his own units (he is 100 tall, his
 * middle is 0 across), then shaded pixel by pixel from where the light falls:
 * from the upper left, with a darker rim round every shape. */

#include <algorithm>
#include <cmath>

#include "figure.h"
#include "figure_art.h"
#include "shapes.h"

namespace wizard {

namespace {

// The hat's cone: where its middle is at a height, and how wide it is. The tip bends to the right.
float HatMiddle(float Y)
{
    return 11.0f * std::pow(1.0f - Clamped(Y / 27.0f), 2.4f);
}

float HatHalf(float Y)
{
    return 1.8f + 18.7f * std::pow(Clamped(Y / 27.0f), 1.1f);
}

// A star of four points at X, Y, Radius wu across.
void Spark(Sprite &Out, const Grid &On, float X, float Y, float Radius, Color Ink)
{
    const float Thin = std::max(0.45f, On.Unit * 0.35f);
    FillStroke(Out, On.X(X - Radius), On.Y(Y), On.X(X + Radius), On.Y(Y), Thin, Thin, [&](float, float, float) { return Ink; });
    FillStroke(Out, On.X(X), On.Y(Y - Radius), On.X(X), On.Y(Y + Radius), Thin, Thin, [&](float, float, float) { return Ink; });
}

void DrawFace(Sprite &Out, const Grid &On)
{
    FillEllipse(Out, On.X(0.0f), On.Y(40.0f), 11.6f * On.Unit, 12.6f * On.Unit, [&](float Nx, float Ny, float Inside) {
        const float Wy = 40.0f + Ny * 12.6f;
        float Level = 0.78f - 0.16f * Nx + 0.05f * Ny;
        Level *= 0.64f + 0.36f * Smooth(32.0f, 39.0f, Wy);      // the brim's shadow over the brow
        Color Here = OnRamp(SkinTones, Level);
        const float Cheek = std::max(0.0f, 1.0f - std::hypot(std::fabs(Nx) - 0.58f, Ny - 0.34f) * 3.2f);
        Here = Mixed(Here, Rgb(0xE8, 0x7C, 0x74), Cheek * 0.40f);
        if (Wy > 34.2f && Wy < 36.5f && std::fabs(Nx) < 0.55f && std::fmod(Wy, 1.7f) < 0.5f) Here = Toned(Here, 0.88f);
        return Inside < 1.0f ? Toned(Here, 0.8f) : Here;
    });
    // Sockets for the eyes, and the nose between them.
    for (const float Side : {-1.0f, 1.0f})
        FillEllipse(Out, On.X(Side * 5.0f), On.Y(38.4f), 3.9f * On.Unit, 2.6f * On.Unit,
                    [](float, float Ny, float) { return Mixed(SkinTones[1], SkinTones[0], 0.4f + 0.2f * Ny); });
    FillEllipse(Out, On.X(0.6f), On.Y(43.4f), 3.1f * On.Unit, 4.7f * On.Unit, [&](float Nx, float Ny, float Inside) {
        Color Here = OnRamp(SkinTones, 0.72f - 0.34f * Nx - 0.14f * Ny);
        if (Ny > 0.45f && std::fabs(Nx) > 0.35f) Here = Toned(Here, 0.72f);
        if (Nx < -0.1f && Ny < 0.1f) Here = Mixed(Here, SkinTones[4], 0.25f);
        return Inside < 0.9f ? Toned(Here, 0.85f) : Here;
    });
}

// The hair at his temples and the beard below it, the strands of both running
// the way they fall, each lit a little differently.
void DrawBeard(Sprite &Out, const Grid &On)
{
    for (const float Side : {-1.0f, 1.0f}) {
        FillStroke(Out, On.X(Side * 12.2f), On.Y(33.0f), On.X(Side * 14.0f), On.Y(58.0f), 3.2f * On.Unit, 5.0f * On.Unit, [&](float Along, float Across, float Inside) {
            const float Strand = std::fabs(Wave(Across * 2.2f + Along * 1.5f + Side));
            Color Here = OnRamp(BeardTones, 0.62f + 0.22f * Strand - 0.14f * Across * Side);
            return Inside < 1.0f ? Toned(Here, 0.78f) : Here;
        });
    }
    FillProfile(Out, On, 40.0f, 94.0f,
        [](float Y) {
            const float T = Clamped((Y - 40.0f) / 54.0f);
            const float Half = (13.0f + 13.0f * Wave(0.5f * std::pow(T, 0.65f))) * (1.0f - std::pow(T, 2.4f)) + 1.3f * Wave(Y * 0.19f) * T;
            return Span{-Half, Half};
        },
        [&](float X, float Y, float Across, float Inside) {
            const float T = Clamped((Y - 40.0f) / 54.0f);
            const float Strand = X + 2.8f * Wave(Y * 0.013f + X * 0.005f) + T * X * 0.18f;
            const float Place = Strand * 0.85f;
            const float Fraction = Place - std::floor(Place);
            const float Locks = Grain(static_cast<int>(std::floor(Place)), 4, 201);
            float Level = 0.70f + 0.24f * std::fabs(Fraction - 0.5f) * 2.0f + 0.12f * Locks - 0.16f * Across;
            if (Fraction < 0.09f) Level -= 0.17f;
            Level *= 0.66f + 0.34f * Smooth(40.0f, 58.0f, Y);                                      // the shadow of his chin
            Level += 0.10f * Smooth(70.0f, 90.0f, Y) * (1.0f - std::fabs(Across));                 // the ends catch the light
            Color Here = OnRamp(BeardTones, Level);
            return Inside < 1.1f ? Toned(Here, 0.80f) : Here;
        });
    // The moustache: two thick curls, a shadow under them, the ends turned up.
    for (const float Side : {-1.0f, 1.0f}) {
        FillStroke(Out, On.X(Side * 0.6f), On.Y(48.6f), On.X(Side * 10.5f), On.Y(49.8f), 3.2f * On.Unit, 1.4f * On.Unit,
                   [](float, float, float) { return Rgb(0x70, 0x76, 0x98); });
        FillStroke(Out, On.X(Side * 0.6f), On.Y(47.4f), On.X(Side * 10.5f), On.Y(48.6f), 3.2f * On.Unit, 1.4f * On.Unit, [&](float Along, float Across, float Inside) {
            Color Here = OnRamp(BeardTones, 0.78f + 0.12f * (1.0f - Along) - 0.18f * Across);
            return Inside < 0.9f ? Toned(Here, 0.82f) : Here;
        });
        FillStroke(Out, On.X(Side * 10.5f), On.Y(48.4f), On.X(Side * 12.8f), On.Y(46.0f), 1.4f * On.Unit, 0.6f * On.Unit,
                   [](float, float, float) { return BeardTones[3]; });
    }
}

// The brim of the hat and the cone that rises from it, its sash and its stars.
void DrawHat(Sprite &Out, const Grid &On)
{
    const auto Brim = [&](float Cx, float Cy, float Rx, float Ry) {
        FillEllipse(Out, On.X(Cx), On.Y(Cy), Rx * On.Unit, Ry * On.Unit, [&](float Nx, float Ny, float Inside) {
            Color Here = OnRamp(HatTones, 0.50f - 0.26f * Nx - 0.40f * Ny);
            if (Ny > 0.50f) Here = OnRamp(HatTones, 0.12f + 0.1f * Nx);
            if (Inside < 1.1f) Here = Toned(Here, 0.66f);
            return Here;
        });
    };
    Brim(0.0f, 28.5f, 28.0f, 5.4f);
    Brim(-22.0f, 31.2f, 7.0f, 2.8f);
    Brim(22.0f, 31.2f, 7.0f, 2.8f);
    FillProfile(Out, On, 0.0f, 31.5f,
        [](float Y) { return Span{HatMiddle(Y) - HatHalf(Y), HatMiddle(Y) + HatHalf(Y)}; },
        [&](float X, float Y, float Across, float Inside) {
            const float Fold = 0.11f * Wave(Across * 2.1f + Y * 0.016f) + 0.05f * Wave(Across * 5.0f - Y * 0.03f);
            float Level = 0.52f - 0.36f * Across + Fold + 0.07f * (Grain(static_cast<int>(X * 2.0f), static_cast<int>(Y * 2.0f), 211) - 0.5f);
            Color Here = OnRamp(HatTones, Level);
            if (Y > 20.6f && Y < 24.6f) Here = OnRamp(GoldTones, 0.50f - 0.28f * Across + 0.08f * Wave(X * 0.5f));
            if (Y > 24.6f && Y < 25.4f) Here = OnRamp(HatTones, 0.2f);
            return Inside < 1.2f ? Toned(Here, 0.72f + 0.1f * Inside) : Here;
        },
        [](float X, float Y) {
            const float Across = (X - HatMiddle(Y)) / HatHalf(Y);
            return Y < 28.5f + 3.6f * std::sqrt(std::max(0.0f, 1.0f - Across * Across));
        });
    const struct { float Across, Down, Radius; } Stars[] = {{-0.46f, 9.5f, 1.7f}, {0.34f, 14.5f, 2.1f}, {-0.16f, 18.2f, 1.5f}, {0.62f, 8.0f, 1.1f}};
    for (const auto &One : Stars)
        Spark(Out, On, HatMiddle(One.Down) + One.Across * HatHalf(One.Down), One.Down, One.Radius, GoldTones[3]);
}

}  // namespace

void Figure::Build(const Scene &Where, const Look &)
{
    const Grid On{Where.Unit};
    Front = Sprite(On.Width(), On.Height());
    DrawFace(Front, On);
    DrawBeard(Front, On);
    DrawHat(Front, On);
    Back = Sprite(On.Width(), On.Height());
    DrawRobe(Back, On);
}

}  // namespace wizard
