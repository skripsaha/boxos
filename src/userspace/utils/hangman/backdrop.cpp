/* backdrop.cpp — the sky, the mountains and the hill, by the pixel (backdrop.h). */

#include "backdrop.h"

#include <cmath>
#include <vector>

#include "numbers.h"

namespace hangman {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

float RidgeAt(float X)
{
    return 60.0f + 7.0f * Wave(X / 46.0f + 0.15f) + 4.0f * Wave(X / 17.0f + 0.6f);
}

// The star at a pixel, if there is one: how bright it shines, 0 for none.
float StarAt(int X, int Y, float Dusk, std::uint64_t Now)
{
    if (Dusk < StarsFrom || Hash(X, Y, 7) % 131 != 0) return 0.0f;
    const float Twinkle = 0.55f + 0.45f * Wave(static_cast<float>(Now % 2400) / 2400.0f + Grain(X, Y, 3));
    return Smooth(StarsFrom, 1.0f, Dusk) * Twinkle;
}

}  // namespace

float HillAt(float X, const Timbers &Wood)
{
    const float Rolling = 80.0f + 4.0f * Wave(X / 70.0f + 0.1f) + 2.0f * Wave(X / 23.0f + 0.4f);
    const float Level = 1.0f - Smooth(30.0f, 46.0f, std::fabs(X - Wood.Pivot));
    return Rolling + (Timbers::StageTop + 3.0f - Rolling) * Level;
}

float WindAt(std::uint64_t Now)
{
    return 0.65f + 0.35f * Wave(static_cast<float>(Now % 9000) / 9000.0f);
}

void PaintBackdrop(tui::Frame &Out, const View &Where, const Timbers &Wood, const Tones &Tone, float Dusk, std::uint64_t Now)
{
    const int Columns = static_cast<int>(Where.Wide);
    const int Rows = static_cast<int>(Where.High);
    const int Left = static_cast<int>(Where.Left);
    const int Top = static_cast<int>(Where.Top);

    std::vector<float> Ridges(static_cast<std::size_t>(Columns));
    std::vector<float> Hills(static_cast<std::size_t>(Columns));
    for (int Column = 0; Column < Columns; Column++) {
        const float X = (static_cast<float>(Column) + 0.5f) / Where.Scale;
        Ridges[static_cast<std::size_t>(Column)] = RidgeAt(X);
        Hills[static_cast<std::size_t>(Column)] = HillAt(X, Wood);
    }
    for (int Row = 0; Row < Rows; Row++) {
        const float Y = (static_cast<float>(Top + Row) + 0.5f - Where.Ground) / Where.Scale;
        const Color Sky = Mixed(Tone.SkyTop, Tone.SkyLow, Smooth(-10.0f, 72.0f, Y));
        const Color Range = Mixed(Tone.Far, Tone.HillDeep, Clamped((Y - 50.0f) / 60.0f));
        for (int Column = 0; Column < Columns; Column++) {
            const float RidgeY = Ridges[static_cast<std::size_t>(Column)];
            const float HillY = Hills[static_cast<std::size_t>(Column)];
            Color       Ink = Sky;
            if (Y >= HillY) {
                const float Depth = Clamped((Y - HillY) / 22.0f);
                const float Rough = Grain(Left + Column, Top + Row) - 0.5f;
                Ink = Mixed(Mixed(Tone.Hill, Tone.HillDeep, Depth), Rough > 0 ? Lighter(Tone.Hill, 0.3f) : Darker(Tone.Hill, 0.3f),
                            std::fabs(Rough) * 0.35f);
                if (Y - HillY < 1.6f) Ink = Lighter(Ink, 0.22f);
            } else if (Y >= RidgeY) {
                Ink = Y - RidgeY < 2.4f ? Mixed(Range, Tone.SkyLow, 0.55f) : Range;
            } else if (const float Star = StarAt(Left + Column, Top + Row, Dusk, Now); Star > 0.0f) {
                Ink = Mixed(Sky, Rgb(255, 250, 230), Star);
            }
            Out.PutPixel({Left + Column, Top + Row}, Ink);
        }
    }
}

void PaintSun(tui::FineCanvas &Fine, const View &Where, const Tones &Tone, float Dusk, float Joy, std::uint64_t Now)
{
    const float X = 22.0f;
    const float Y = 20.0f + 26.0f * Dusk;
    const float Radius = 6.5f + 2.5f * Dusk + 1.0f * Joy;
    const core::PointF Centre = Where.At(X, Y);
    Fine.Disc(Centre.X, Centre.Y, Where.Span(Radius * 2.6f), Tone.Sun, 0.10f);
    Fine.Disc(Centre.X, Centre.Y, Where.Span(Radius * 1.7f), Tone.Sun, 0.16f);
    const float Turn = static_cast<float>(Now % 40000) / 40000.0f;
    const float Reach = 3.0f + 2.5f * Joy + 1.2f * Wave(static_cast<float>(Now % 1800) / 1800.0f);
    for (int Ray = 0; Ray < 12; Ray++) {
        const float Angle = (static_cast<float>(Ray) / 12.0f + Turn) * 6.2831853f;
        const float Cos = std::cos(Angle), Sin = std::sin(Angle);
        const core::PointF From{Centre.X + Where.Span((Radius + 1.6f) * Cos), Centre.Y + Where.Span((Radius + 1.6f) * Sin)};
        const core::PointF To{Centre.X + Where.Span((Radius + 1.6f + Reach) * Cos), Centre.Y + Where.Span((Radius + 1.6f + Reach) * Sin)};
        Fine.Line(From, To, std::max(0.7f, Where.Span(1.0f)), Tone.Sun, 0.6f * (1.0f - 0.5f * Dusk));
    }
    Fine.Disc(Centre.X, Centre.Y, Where.Span(Radius), Tone.Sun);
    Fine.Disc(Centre.X - Where.Span(Radius * 0.25f), Centre.Y - Where.Span(Radius * 0.25f), Where.Span(Radius * 0.55f),
              Lighter(Tone.Sun, 0.35f), 0.55f);
}

void PaintClouds(tui::FineCanvas &Fine, const View &Where, const Tones &Tone, std::uint64_t Now)
{
    struct Cloud {
        float Start, Up, Size, Pace;
    };
    static constexpr Cloud Drifting[] = {{12.0f, 14.0f, 1.0f, 2.4f}, {64.0f, 27.0f, 0.75f, 1.5f}, {40.0f, 8.0f, 0.6f, 1.9f}, {90.0f, 38.0f, 0.9f, 1.1f}};
    const float Run = Where.Across + 60.0f;
    for (const Cloud &Each : Drifting) {
        const float Travelled = Each.Start + static_cast<float>(Now % 400000) / 1000.0f * Each.Pace;
        const float X = std::fmod(Travelled, Run) - 30.0f;
        const float S = Each.Size;
        const auto  Puff = [&](float Dx, float Dy, float Wide, float High, float Opacity, Color Ink) {
            const core::PointF At = Where.At(X + Dx * S, Each.Up + Dy * S);
            Fine.Ellipse(At.X, At.Y, Where.Span(Wide * S), Where.Span(High * S), Ink, Opacity);
        };
        Puff(0.0f, 1.2f, 10.0f, 3.0f, 0.22f, Darker(Tone.Cloud, 0.35f));
        Puff(0.0f, 0.0f, 9.0f, 3.2f, 0.95f, Tone.Cloud);
        Puff(-6.0f, 1.0f, 6.0f, 2.6f, 0.95f, Tone.Cloud);
        Puff(6.5f, 1.2f, 6.5f, 2.5f, 0.95f, Tone.Cloud);
        Puff(1.0f, -2.4f, 5.0f, 3.0f, 0.95f, Tone.Cloud);
    }
}

void PaintBirds(tui::FineCanvas &Fine, const View &Where, const Tones &Tone, float Joy, std::uint64_t Now)
{
    if (Joy < 0.15f) return;
    for (int Bird = 0; Bird < 3; Bird++) {
        const float Travelled = static_cast<float>(Now % 50000) / 1000.0f * (6.0f + static_cast<float>(Bird)) + static_cast<float>(Bird) * 14.0f;
        const float X = std::fmod(Travelled, Where.Across + 30.0f) - 15.0f;
        const float Y = 18.0f + static_cast<float>(Bird) * 7.0f + 1.5f * Wave(static_cast<float>(Now % 3000) / 3000.0f + static_cast<float>(Bird) * 0.3f);
        const float Flap = 2.2f * Wave(static_cast<float>(Now % 520) / 520.0f + static_cast<float>(Bird) * 0.2f);
        const core::PointF Body = Where.At(X, Y);
        const float Thin = std::max(0.6f, Where.Span(0.9f));
        Fine.Line(Where.At(X - 3.0f, Y - Flap), Body, Thin, Tone.Figure, Joy);
        Fine.Line(Body, Where.At(X + 3.0f, Y - Flap), Thin, Tone.Figure, Joy);
    }
}

void PaintTree(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now)
{
    const float Ground = HillAt(TreeAt, Wood);
    const core::PointF Foot = Where.At(TreeAt - 1.8f, Ground - 25.0f);
    Fine.Rounded(Foot.X, Foot.Y, Where.Span(3.6f), Where.Span(26.0f), Where.Span(0.8f), Tone.Trunk);
    const float Gust = WindAt(Now);
    struct Crown {
        float Dx, Up, Radius, Sway;
    };
    static constexpr Crown Leaves[] = {{0.0f, 28.0f, 8.5f, 1.0f}, {-6.5f, 23.0f, 6.5f, 0.7f}, {6.8f, 23.5f, 6.8f, 0.8f}, {0.5f, 34.5f, 6.0f, 1.2f}};
    for (const Crown &Each : Leaves) {
        const float Sway = 1.1f * Gust * Each.Sway * Wave(static_cast<float>(Now % 2300) / 2300.0f + Each.Dx * 0.05f);
        const core::PointF At = Where.At(TreeAt + Each.Dx + Sway, Ground - Each.Up);
        Fine.Disc(At.X, At.Y + Where.Span(0.9f), Where.Span(Each.Radius), Darker(Tone.Leaf, 0.35f));
        Fine.Disc(At.X, At.Y, Where.Span(Each.Radius), Tone.Leaf);
        Fine.Disc(At.X - Where.Span(Each.Radius * 0.3f), At.Y - Where.Span(Each.Radius * 0.3f), Where.Span(Each.Radius * 0.55f),
                  Lighter(Tone.Leaf, 0.22f), 0.7f);
    }
}

void PaintGrass(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now)
{
    const float Gust = WindAt(Now);
    const Color Blade = Lighter(Tone.Hill, 0.25f);
    const float Thin = std::max(0.6f, Where.Span(0.7f));
    for (float X = 1.0f; X < Where.Across; X += 3.6f) {
        const float Ground = HillAt(X, Wood);
        const float Bend = 1.6f * Gust * Wave(static_cast<float>(Now % 1100) / 1100.0f + X * 0.07f);
        const float Tall = 2.6f + 1.2f * Grain(static_cast<int>(X), 5);
        Fine.Line(Where.At(X, Ground + 0.4f), Where.At(X + Bend, Ground - Tall), Thin, Blade, 0.8f);
    }
}

}  // namespace hangman
