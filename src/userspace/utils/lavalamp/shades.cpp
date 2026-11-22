/* shades.cpp — the liquid, the glass and the wax. See shades.h. */

#include "shades.h"

#include <algorithm>
#include <cmath>

namespace lavalamp {

namespace {

constexpr Rgb White{1, 1, 1};

Rgb Of(Color From)
{
    return {static_cast<float>(From.Red()) / 255.0f, static_cast<float>(From.Green()) / 255.0f, static_cast<float>(From.Blue()) / 255.0f};
}

float Smooth(float Edge0, float Edge1, float X)
{
    const float T = std::clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
    return T * T * (3 - 2 * T);
}

float Bell(float X, float Middle, float Width)
{
    const float D = (X - Middle) / Width;
    return std::exp(-D * D);
}

/* The light from the top left and before the glass, and halfway between it
 * and the eye. */
constexpr float LightX = -0.45f, LightY = -0.62f, LightZ = 0.64f;

}  // namespace

Light LightOf(float Level, float SlopeX, float SlopeY)
{
    const float Flat = 1.0f / ((1.0f + Level) * (1.0f + Level));
    const float Nx = -SlopeX * Flat, Ny = -SlopeY * Flat;
    const float Inverse = 1.0f / std::sqrt(Nx * Nx + Ny * Ny + 1.0f);
    const float X = Nx * Inverse, Y = Ny * Inverse, Z = Inverse;
    const float Lx = LightX, Ly = LightY, Lz = LightZ + 1.0f;   // the light and the eye halved
    const float Half   = 1.0f / std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
    float       Facing = std::max(0.0f, (X * Lx + Y * Ly + Z * Lz) * Half);
    for (int Times = 0; Times < 6; Times++) Facing *= Facing;   // to the 64th: a small spot, none on the flat
    const float Spot = Facing;
    Light Made;
    Made.Lit   = 0.64f + 0.42f * std::max(0.0f, X * LightX + Y * LightY + Z * LightZ);
    Made.Shine = Spot;
    Made.Under = (1.0f - Z) * std::max(0.0f, Y);
    return Made;
}

Shades::Shades(const Composition &Made)
    : Hot(Of(Made.Wax)), Cool(Of(Made.Rim)), Bright(Mix(Of(Made.Wax), White, 0.2f)), Deep(Of(Made.Glass)),
      Lamp(Mix(Of(Made.Wax), White, 0.3f)), Glow(static_cast<float>(Made.Glow) / 100.0f), Lit(Made.Light)
{
}

Rgb Shades::Liquid(float Y) const
{
    if (!Lit) return Deep;
    const Rgb Top    = Times(Deep, 0.45f);
    const Rgb Warmed = Times(Mix(Deep, Hot, 0.3f), 1.25f);
    const Rgb Middle = Mix(Top, Deep, Smooth(0.0f, 0.55f, Y));
    const float Low  = Smooth(0.5f, 1.0f, Y);
    return Mix(Middle, Warmed, Low * Low * 0.75f);
}

float Shades::Round(float U) const
{
    if (!Lit) return 1;
    return 0.62f + 0.38f * std::sqrt(std::max(0.0f, std::sin(3.14159265f * std::clamp(U, 0.0f, 1.0f))));
}

float Shades::BulbAcross(float U) const
{
    if (!Lit) return 0;
    return 0.5f * Bell(U, 0.5f, 0.32f);
}

float Shades::BulbUp(float Y) const
{
    return std::exp(-(1.0f - Y) / 0.16f);
}

Rgb Shades::Wax(float Heat, float Level, const Light &Lit, float Y) const
{
    Rgb Body = Mix(Cool, Hot, Smooth(0.08f, 0.92f, Heat));
    if (!this->Lit) return Body;
    Body     = Mix(Body, Bright, 0.35f * Smooth(1.3f, 3.4f, Level));
    Body     = Times(Body, (1.0f + 0.12f * Smooth(0.55f, 1.0f, Y)) * Lit.Lit);
    return Plus(Plus(Body, Times(Bright, 0.35f * Lit.Under)), Times(White, 0.65f * Lit.Shine));
}

}  // namespace lavalamp
