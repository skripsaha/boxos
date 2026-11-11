/* figure_robe.cpp — the robe: pleats falling from the shoulders and widening
 * to the hem, a gold cord at the waist with its tassel, the trim and the stars
 * embroidered on it, and the pointed shoes beneath. */

#include <algorithm>
#include <cmath>

#include "figure_art.h"
#include "shapes.h"

namespace wizard {

namespace {

// How wide the robe is at a height: the shoulders, then the long fall of the skirt.
float RobeHalf(float Y)
{
    if (Y < 57.0f) return 9.0f + (Y - 50.0f) * 2.3f;
    return 25.1f + (Y - 57.0f) * 0.30f;
}

void DrawShoes(Sprite &Out, const Grid &On)
{
    for (const float Side : {-1.0f, 1.0f}) {
        FillEllipse(Out, On.X(Side * 11.0f), On.Y(101.5f), 8.0f * On.Unit, 3.3f * On.Unit, [&](float Nx, float Ny, float Inside) {
            Color Here = OnRamp(LeatherTones, 0.52f - 0.24f * Nx - 0.36f * Ny);
            return Inside < 0.9f ? Toned(Here, 0.7f) : Here;
        });
        FillStroke(Out, On.X(Side * 15.0f), On.Y(101.8f), On.X(Side * 21.0f), On.Y(100.4f), 2.2f * On.Unit, 0.5f * On.Unit,
                   [&](float, float Across, float) { return OnRamp(LeatherTones, 0.5f - 0.3f * Across); });
    }
}

// The cord round his waist, hanging in a tassel on his left.
void DrawCord(Sprite &Out, const Grid &On)
{
    const float Thick = std::max(0.9f, On.Unit * 1.5f);
    FillStroke(Out, On.X(-25.0f), On.Y(64.0f), On.X(26.0f), On.Y(64.6f), Thick, Thick, [&](float, float Across, float) { return OnRamp(GoldTones, 0.55f - 0.3f * Across); });
    FillStroke(Out, On.X(-12.0f), On.Y(64.6f), On.X(-14.0f), On.Y(79.0f), Thick * 0.7f, Thick * 0.7f, [&](float, float Across, float) { return OnRamp(GoldTones, 0.5f - 0.3f * Across); });
    FillStroke(Out, On.X(-14.0f), On.Y(78.0f), On.X(-14.2f), On.Y(84.0f), Thick * 1.6f, Thick * 0.5f, [&](float Along, float Across, float) { return OnRamp(GoldTones, 0.7f - 0.3f * Across - 0.2f * Along); });
}

void DrawTrim(Sprite &Out, const Grid &On)
{
    FillProfile(Out, On, 96.2f, 100.0f, [](float) { return Span{-RobeHalf(100.0f), RobeHalf(100.0f)}; },
                [&](float X, float Y, float, float Inside) {
                    Color Here = OnRamp(GoldTones, 0.52f - 0.40f * (Y - 96.2f) / 3.8f + 0.10f * Wave(X * 0.6f));
                    if (std::fmod(std::fabs(X) + 3.5f, 7.0f) < 1.3f && Y > 97.0f && Y < 99.2f) Here = OnRamp(HatTones, 0.4f);
                    return Inside < 0.8f ? Toned(Here, 0.8f) : Here;
                });
}

}  // namespace

void DrawRobe(Sprite &Out, const Grid &On)
{
    DrawShoes(Out, On);
    FillProfile(Out, On, 50.0f, 100.0f, [](float Y) { return Span{-RobeHalf(Y), RobeHalf(Y)}; },
                [&](float X, float Y, float Across, float Inside) {
                    const float Pleat = Wave(X * 0.085f * (1.0f + (Y - 50.0f) * 0.004f) + 0.10f * Wave(Y * 0.02f));
                    float Level = 0.48f + 0.22f * Pleat - 0.17f * Across + 0.06f * (Grain(static_cast<int>(X * 1.5f), static_cast<int>(Y * 1.5f), 221) - 0.5f);
                    Level += 0.05f * Smooth(54.0f, 70.0f, Y) * (1.0f - Smooth(70.0f, 90.0f, Y));
                    Color Here = OnRamp(RobeTones, Level);
                    return Inside < 1.2f ? Toned(Here, 0.62f + 0.12f * Inside) : Here;
                });
    DrawCord(Out, On);
    DrawTrim(Out, On);
    const struct { float X, Y, Radius; } Stars[] = {{-28.0f, 83.0f, 1.9f}, {29.0f, 88.0f, 2.2f}, {-31.0f, 94.0f, 1.5f}, {24.0f, 72.0f, 1.6f}, {31.0f, 80.0f, 1.2f}};
    const float Thin = std::max(0.45f, On.Unit * 0.35f);
    for (const auto &One : Stars) {
        FillStroke(Out, On.X(One.X - One.Radius), On.Y(One.Y), On.X(One.X + One.Radius), On.Y(One.Y), Thin, Thin, [](float, float, float) { return GoldTones[3]; });
        FillStroke(Out, On.X(One.X), On.Y(One.Y - One.Radius), On.X(One.X), On.Y(One.Y + One.Radius), Thin, Thin, [](float, float, float) { return GoldTones[3]; });
    }
}

}  // namespace wizard
