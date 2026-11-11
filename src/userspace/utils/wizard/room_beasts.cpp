/* room_beasts.cpp — the owl on its perch and the cat by the fire: each
 * breathes, blinks and keeps an eye on what goes on. */

#include <algorithm>
#include <cmath>

#include "room_parts.h"
#include "shapes.h"

namespace wizard {

namespace {

Color Lit(Color Ink, const Tint &Light)
{
    return Scaled(Ink, Light.Red, Light.Green, Light.Blue);
}

constexpr Color Feather[] = {Rgb(0x2E, 0x22, 0x18), Rgb(0x58, 0x40, 0x2A), Rgb(0x86, 0x66, 0x40), Rgb(0xB0, 0x8E, 0x5C)};
constexpr Color Ginger[] = {Rgb(0x6A, 0x32, 0x14), Rgb(0xA8, 0x58, 0x20), Rgb(0xD8, 0x84, 0x34), Rgb(0xF4, 0xB4, 0x68)};

// The owl's eye: round and yellow, the pupil turned to Toward (-1..1), shut for a moment now and then.
void DrawEye(Canvas &Out, float X, float Y, float Radius, float Toward, bool Shut, const Tint &Light)
{
    if (Shut) {
        Out.Cover(static_cast<int>(X - Radius * 0.6f), static_cast<int>(Y), Lit(Feather[0], Light), 1.0f);
        Out.Cover(static_cast<int>(X + Radius * 0.6f), static_cast<int>(Y), Lit(Feather[0], Light), 1.0f);
        return;
    }
    FillEllipse(Out, X, Y, Radius, Radius, [&](float Nx, float Ny, float) {
        const float Dx = Nx - Toward * 0.35f;
        return Dx * Dx + Ny * Ny < 0.28f ? Rgb(0x10, 0x0C, 0x08) : Lit(Mixed(Rgb(0xFF, 0xC8, 0x30), Rgb(0xFF, 0xF0, 0x90), (Ny < -0.3f) ? 0.5f : 0.0f), Light);
    });
}

}  // namespace

void DrawOwl(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights)
{
    const float Size = std::max(6.0f, Where.Unit * 22.0f);
    const float Cx = static_cast<float>(Where.OwlX), Foot = static_cast<float>(Where.OwlY);
    const Tint Light = Lights.At(Where.OwlX, Where.OwlY - static_cast<int>(Size * 0.5f));
    const float Breath = 1.0f + 0.035f * Wave(static_cast<float>(Now.Ms % 3600) / 3600.0f);
    const bool  Shut = Now.Ms % 5200u < 170u;
    const float Toward = std::clamp(static_cast<float>(Where.WizardX - Where.OwlX) / static_cast<float>(Where.Width) * 3.0f, -1.0f, 1.0f);
    const bool Tilt = Now.Ms % 9000u > 8500u;

    const float BodyRy = Size * 0.40f * Breath;
    FillEllipse(Out, Cx, Foot - BodyRy - 0.5f, Size * 0.32f, BodyRy, [&](float Nx, float Ny, float Inside) {
        float Level = 0.55f - 0.30f * Nx + 0.10f * Ny + 0.14f * Grain(static_cast<int>(Nx * 12.0f + 20.0f), static_cast<int>(Ny * 12.0f + 20.0f), 111);
        Color Here = OnRamp(Feather, Level);
        if (Ny > -0.2f && std::fabs(Nx) < 0.55f)
            Here = Mixed(Here, Rgb(0xE0, 0xD2, 0xB0), Grain(static_cast<int>(Nx * 9.0f + 20.0f), static_cast<int>(Ny * 9.0f + 20.0f), 112) > 0.62f ? 0.45f : 0.78f);
        if (Inside < 1.0f) Here = Darker(Here, 60);
        return Lit(Here, Light);
    });
    const float HeadX = Cx + (Tilt ? 1.0f : 0.0f), HeadY = Foot - Size * 0.78f;
    FillEllipse(Out, HeadX, HeadY, Size * 0.31f, Size * 0.26f, [&](float Nx, float Ny, float Inside) {
        Color Here = OnRamp(Feather, 0.62f - 0.30f * Nx + 0.1f * Ny);
        if (Ny > -0.3f && Nx * Nx * 1.4f + (Ny - 0.1f) * (Ny - 0.1f) < 0.62f) Here = Mixed(Here, Rgb(0xE8, 0xDA, 0xBC), 0.55f);
        if (Inside < 1.0f) Here = Darker(Here, 60);
        return Lit(Here, Light);
    });
    FillStroke(Out, HeadX - Size * 0.22f, HeadY - Size * 0.18f, HeadX - Size * 0.30f, HeadY - Size * 0.40f, 1.1f, 0.4f,
               [&](float, float, float) { return Lit(Feather[1], Light); });
    FillStroke(Out, HeadX + Size * 0.22f, HeadY - Size * 0.18f, HeadX + Size * 0.30f, HeadY - Size * 0.40f, 1.1f, 0.4f,
               [&](float, float, float) { return Lit(Feather[1], Light); });
    const float Eye = std::max(1.2f, Size * 0.105f);
    DrawEye(Out, HeadX - Size * 0.13f, HeadY, Eye, Toward, Shut, Light);
    DrawEye(Out, HeadX + Size * 0.13f, HeadY, Eye, Toward, Shut, Light);
    Out.Cover(static_cast<int>(HeadX), static_cast<int>(HeadY + Size * 0.10f), Lit(Rgb(0xE8, 0x92, 0x28), Light), 1.0f);
    Out.Cover(static_cast<int>(Cx - Size * 0.12f), static_cast<int>(Foot), Lit(Rgb(0xD8, 0x84, 0x28), Light), 1.0f);
    Out.Cover(static_cast<int>(Cx + Size * 0.12f), static_cast<int>(Foot), Lit(Rgb(0xD8, 0x84, 0x28), Light), 1.0f);
}

void DrawCat(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights)
{
    const float Rx = std::max(6.0f, Where.Unit * 17.0f), Ry = std::max(3.2f, Where.Unit * 8.5f);
    const float Cx = static_cast<float>(Where.CatX), Floor = static_cast<float>(Where.CatY);
    const Tint Light = Lights.At(Where.CatX - static_cast<int>(Rx * 0.5f), Where.CatY - static_cast<int>(Ry));
    const float Breath = 1.0f + 0.07f * Wave(static_cast<float>(Now.Ms % 2900) / 2900.0f);
    const bool Awake = Now.Excite > 0.35f;
    const bool Twitch = Now.Ms % 7300u < 160u;

    // The tail curls round the front of him, the tip darker.
    const float TailY = Floor - 1.3f;
    FillStroke(Out, Cx + Rx * 0.80f, Floor - Ry * 0.9f, Cx + Rx * 0.62f, TailY, 1.9f, 1.6f, [&](float, float Across, float) {
        return Lit(OnRamp(Ginger, 0.45f - 0.2f * Across), Light);
    });
    FillStroke(Out, Cx + Rx * 0.62f, TailY, Cx - Rx * 0.55f, TailY, 1.6f, 1.5f, [&](float Along, float Across, float) {
        Color Here = OnRamp(Ginger, 0.50f - 0.25f * Across);
        if (Along > 0.80f) Here = Darker(Here, 90);
        return Lit(Here, Light);
    });
    FillEllipse(Out, Cx, Floor - Ry * Breath, Rx, Ry * Breath, [&](float Nx, float Ny, float Inside) {
        const float Stripe = Wave(Nx * 4.2f + 0.25f * Ny) > 0.55f ? 0.22f : 0.0f;
        Color Here = OnRamp(Ginger, 0.62f - 0.34f * Nx - 0.22f * Ny - Stripe + 0.08f * Grain(static_cast<int>(Nx * 14.0f + 30.0f), static_cast<int>(Ny * 14.0f + 30.0f), 121));
        if (Inside < 1.0f) Here = Darker(Here, 70);
        return Lit(Here, Light);
    });
    const float HeadX = Cx - Rx * 0.72f, HeadY = Floor - Ry * 0.78f;
    const float Head = Ry * 0.66f;
    FillStroke(Out, HeadX - Head * 0.75f, HeadY - Head * 0.5f, HeadX - Head * 0.55f - (Twitch ? 0.8f : 0.0f), HeadY - Head * 1.45f, 1.4f, 0.4f,
               [&](float, float, float) { return Lit(Ginger[1], Light); });
    FillStroke(Out, HeadX + Head * 0.35f, HeadY - Head * 0.7f, HeadX + Head * 0.45f, HeadY - Head * 1.5f, 1.4f, 0.4f,
               [&](float, float, float) { return Lit(Ginger[1], Light); });
    FillEllipse(Out, HeadX, HeadY, Head, Head * 0.85f, [&](float Nx, float Ny, float Inside) {
        Color Here = OnRamp(Ginger, 0.70f - 0.30f * Nx - 0.15f * Ny);
        if (Ny > 0.25f && std::fabs(Nx) < 0.5f) Here = Mixed(Here, Ginger[3], 0.5f);
        if (Inside < 1.0f) Here = Darker(Here, 70);
        return Lit(Here, Light);
    });
    const float EyeY = HeadY - Head * 0.05f;
    for (const float Side : {-0.40f, 0.40f}) {
        if (Awake) Out.Cover(static_cast<int>(HeadX + Head * Side), static_cast<int>(EyeY), Lit(Rgb(0xE8, 0xE0, 0x40), Light), 1.0f);
        else Out.Cover(static_cast<int>(HeadX + Head * Side), static_cast<int>(EyeY), Lit(Ginger[0], Light), 1.0f);
    }
    Out.Cover(static_cast<int>(HeadX - Head * 0.05f), static_cast<int>(HeadY + Head * 0.28f), Lit(Rgb(0xE0, 0x88, 0x88), Light), 1.0f);
}

}  // namespace wizard
