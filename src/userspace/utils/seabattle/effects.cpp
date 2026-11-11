/* effects.cpp — the pictures of a shell, a splash, a blast, bubbles and a firework (effects.h). */

#include "effects.h"

#include <algorithm>
#include <cmath>

namespace seabattle {

namespace {

constexpr float Pi = 3.14159265f;
constexpr float TwoPi = 6.2831853f;

float AgeOf(const Effect &One, std::uint64_t Now)
{
    return Now <= One.Born ? 0.0f : Clamped(static_cast<float>(Now - One.Born) / static_cast<float>(One.Life));
}

float SecondsOf(const Effect &One, std::uint64_t Now)
{
    return Now <= One.Born ? 0.0f : static_cast<float>(Now - One.Born) * 0.001f;
}

// A number from 0 to 1 that is the same for one effect's Part-th piece, always.
float Piece(const Effect &One, int Part, std::uint32_t Salt = 0)
{
    return Grain(static_cast<int>(One.Born & 0xFFFF), Part, Salt);
}

// Where a shell is at a moment of its flight, 0..1: straight between the two points, and lifted by an arc.
painter::core::PointF ShellAt(const Effect &One, float T)
{
    const float Lift = std::max(One.Reach * 3.0f, 0.32f * std::fabs(One.X - One.FromX) + 0.12f * std::fabs(One.Y - One.FromY));
    return {One.FromX + (One.X - One.FromX) * T, One.FromY + (One.Y - One.FromY) * T - Lift * 4.0f * T * (1.0f - T)};
}

void ShellPixels(const Effect &One, Canvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now);
    if (Age < 0.18f) Out.Glow(One.FromX, One.FromY, One.Reach * 1.8f, Rgb(0xFF, 0xE0, 0x90), static_cast<int>(210.0f * (1.0f - Age / 0.18f)));
    for (int Trail = 1; Trail <= 7; Trail++) {
        const float Behind = Age - static_cast<float>(Trail) * 0.022f;
        if (Behind <= 0.0f) break;
        const painter::core::PointF At = ShellAt(One, Behind);
        Out.Cover(static_cast<int>(At.X), static_cast<int>(At.Y), Rgb(0xFF, 0xD2, 0x80), 0.5f * (1.0f - static_cast<float>(Trail) / 8.0f));
    }
}

void ShellFine(const Effect &One, painter::tui::FineCanvas &Out, std::uint64_t Now)
{
    const float                 Age = AgeOf(One, Now);
    const painter::core::PointF At = ShellAt(One, Age);
    const float                 Size = 0.8f + 0.7f * 4.0f * Age * (1.0f - Age);
    Out.Disc(At.X, At.Y, Size + 0.5f, Rgb(0xFF, 0x9A, 0x40), 0.5f);
    Out.Disc(At.X, At.Y, Size, Rgb(0xFF, 0xF4, 0xD0));
}

void SplashPixels(const Effect &One, Canvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now), Seconds = SecondsOf(One, Now);
    if (Age < 0.32f) {
        const float Rise = One.Reach * 1.7f * std::sin(Age / 0.32f * Pi);
        for (float Up = 0.0f; Up < Rise; Up += 1.0f)
            Out.Cover(static_cast<int>(One.X), static_cast<int>(One.Y - Up), One.Tone, 0.9f * (1.0f - Up / (Rise + 1.0f) * 0.5f));
    }
    for (int Drop = 0; Drop < 10; Drop++) {
        const float Across = (Piece(One, Drop, 1) - 0.5f) * One.Reach * 4.2f, Up = (0.55f + Piece(One, Drop, 2)) * One.Reach * 5.0f;
        const float X = One.X + Across * Seconds, Y = One.Y - Up * Seconds + 0.5f * One.Reach * 17.0f * Seconds * Seconds;
        if (Y <= One.Y + 0.5f) Out.Cover(static_cast<int>(X), static_cast<int>(Y), One.Tone, 0.95f * (1.0f - Age));
    }
}

void SplashFine(const Effect &One, painter::tui::FineCanvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now);
    for (int Ring = 0; Ring < 3; Ring++) {
        const float Q = Clamped((Age - static_cast<float>(Ring) * 0.13f) / 0.62f);
        if (Q <= 0.0f || Q >= 1.0f) continue;
        const float Outer = One.Reach * (0.2f + 0.95f * Q);
        Out.Ring(One.X, One.Y, Outer, std::max(0.0f, Outer - 0.9f), One.Tone, 0.85f * std::pow(1.0f - Q, 1.3f));
    }
}

void BlastPixels(const Effect &One, Canvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now), Seconds = SecondsOf(One, Now);
    if (Age < 0.26f) Out.Glow(One.X, One.Y, One.Reach * 3.4f * (1.0f - Age * 1.5f), Rgb(0xFF, 0xF0, 0xC8), static_cast<int>(230.0f * (1.0f - Age / 0.26f)));
    Out.Glow(One.X, One.Y, One.Reach * 1.9f, One.Tone, static_cast<int>(110.0f * (1.0f - Age)));
    for (int Spark = 0; Spark < 18; Spark++) {
        const float Angle = Piece(One, Spark, 3) * TwoPi, Speed = (1.2f + Piece(One, Spark, 4) * 3.0f) * One.Reach;
        const float X = One.X + std::cos(Angle) * Speed * Seconds, Y = One.Y + std::sin(Angle) * Speed * Seconds + 0.5f * One.Reach * 9.0f * Seconds * Seconds;
        const Color Hot = Mixed(Rgb(0xFF, 0xF0, 0xB0), One.Tone, std::min(1.0f, Age * 3.0f));
        Out.Cover(static_cast<int>(X), static_cast<int>(Y), Hot, std::max(0.0f, 1.0f - Age * 1.7f));
    }
}

void BlastFine(const Effect &One, painter::tui::FineCanvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now);
    const float Fade = 1.0f - Smooth(0.3f, 0.8f, Age);
    const float Ball = One.Reach * (0.2f + 0.75f * Smooth(0.0f, 0.34f, Age));
    if (Fade > 0.0f) {
        Out.Disc(One.X, One.Y, Ball, Mixed(One.Tone, Rgb(0xC8, 0x30, 0x18), Age), Fade);
        Out.Disc(One.X, One.Y, Ball * 0.6f, Rgb(0xFF, 0xD8, 0x60), Fade);
        Out.Disc(One.X, One.Y, Ball * 0.28f, Rgb(0xFF, 0xFF, 0xE8), Fade);
    }
    if (Age < 0.5f) {
        const float Outer = One.Reach * (0.3f + 2.6f * Age);
        Out.Ring(One.X, One.Y, Outer, Outer - 0.8f, Rgb(0xFF, 0xFF, 0xFF), 0.55f * (1.0f - Age * 2.0f));
    }
    for (int Puff = 0; Puff < 4; Puff++) {
        const float Q = Clamped((Age - 0.16f - static_cast<float>(Puff) * 0.1f) / 0.7f);
        if (Q <= 0.0f || Q >= 1.0f) continue;
        const float Sway = (static_cast<float>(Puff) - 1.5f) * One.Reach * 0.22f * Q;
        Out.Disc(One.X + Sway, One.Y - Q * One.Reach * 1.6f, One.Reach * (0.2f + 0.36f * Q), Rgb(0x34, 0x34, 0x3A), 0.6f * (1.0f - Q));
    }
}

void BubblesFine(const Effect &One, painter::tui::FineCanvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now);
    if (Age < 0.9f) Out.Disc(One.X, One.Y, One.Reach * 0.5f + One.Length * 0.25f * Age, Rgb(0x10, 0x14, 0x18), 0.35f * (1.0f - Age));
    for (int Bubble = 0; Bubble < 14; Bubble++) {
        const float Q = Clamped(Age * 1.5f - Piece(One, Bubble, 5) * 0.5f);
        if (Q <= 0.0f || Q >= 1.0f) continue;
        const float Along = (Piece(One, Bubble, 6) - 0.5f) * One.Length, Wobble = Wave(Q * 2.0f + Piece(One, Bubble, 7)) * One.Reach * 0.12f;
        const float X = One.X + Along + Wobble, Y = One.Y - Q * One.Reach * 1.3f;
        Out.Ring(X, Y, 0.6f + Piece(One, Bubble, 8) * 0.7f, 0.25f, Rgb(0xE6, 0xF6, 0xFF), 0.85f * (1.0f - Q));
    }
}

// A rocket climbs for the first third of its life, then bursts into sparks that fall.
constexpr float Climb = 0.34f;

void SalutePixels(const Effect &One, Canvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now), Seconds = SecondsOf(One, Now);
    if (Age < Climb) {
        const float Q = Age / Climb;
        const float Y = One.FromY + (One.Y - One.FromY) * (1.0f - (1.0f - Q) * (1.0f - Q));
        for (float Tail = 0.0f; Tail < 6.0f; Tail += 1.0f) Out.Cover(static_cast<int>(One.X), static_cast<int>(Y + Tail), Rgb(0xFF, 0xD8, 0x90), 0.7f - Tail * 0.11f);
        return;
    }
    const float Burst = (Age - Climb) / (1.0f - Climb), Since = Seconds - Climb * static_cast<float>(One.Life) * 0.001f;
    if (Burst < 0.2f) Out.Glow(One.X, One.Y, One.Reach * 4.0f, One.Tone, static_cast<int>(160.0f * (1.0f - Burst / 0.2f)));
    for (int Spark = 0; Spark < 34; Spark++) {
        const float Angle = Piece(One, Spark, 9) * TwoPi, Speed = (0.45f + Piece(One, Spark, 10) * 0.55f) * One.Reach * 6.5f;
        const float X = One.X + std::cos(Angle) * Speed * Since, Y = One.Y + std::sin(Angle) * Speed * Since + 0.5f * One.Reach * 3.0f * Since * Since;
        Out.Cover(static_cast<int>(X), static_cast<int>(Y), Mixed(One.Tone, Rgb(0xFF, 0xFF, 0xFF), 0.35f * (1.0f - Burst)), 1.0f - Burst * Burst);
    }
}

void SaluteFine(const Effect &One, painter::tui::FineCanvas &Out, std::uint64_t Now)
{
    const float Age = AgeOf(One, Now);
    if (Age < Climb) {
        const float Q = Age / Climb;
        Out.Disc(One.X, One.FromY + (One.Y - One.FromY) * (1.0f - (1.0f - Q) * (1.0f - Q)), 0.8f, Rgb(0xFF, 0xF0, 0xC0));
        return;
    }
    const float Burst = (Age - Climb) / (1.0f - Climb), Since = SecondsOf(One, Now) - Climb * static_cast<float>(One.Life) * 0.001f;
    for (int Spark = 0; Spark < 12; Spark++) {
        const float Angle = (static_cast<float>(Spark) / 12.0f + Piece(One, Spark, 11) * 0.04f) * TwoPi, Speed = One.Reach * 6.8f;
        const float X = One.X + std::cos(Angle) * Speed * Since, Y = One.Y + std::sin(Angle) * Speed * Since + 0.5f * One.Reach * 3.0f * Since * Since;
        Out.Disc(X, Y, 0.9f * (1.0f - Burst * 0.6f), Lighter(One.Tone, 0.3f), 1.0f - Burst * Burst);
    }
}

}  // namespace

void Effects::Prune(std::uint64_t Now)
{
    std::erase_if(Live, [Now](const Effect &One) { return Now >= One.Born + One.Life; });
}

void Effects::DrawPixels(Canvas &Out, std::uint64_t Now) const
{
    for (const Effect &One : Live) {
        switch (One.Kind) {
        case EffectKind::Shell:   ShellPixels(One, Out, Now); break;
        case EffectKind::Splash:  SplashPixels(One, Out, Now); break;
        case EffectKind::Blast:   BlastPixels(One, Out, Now); break;
        case EffectKind::Salute:  SalutePixels(One, Out, Now); break;
        case EffectKind::Bubbles: break;
        }
    }
}

void Effects::DrawFine(painter::tui::FineCanvas &Out, std::uint64_t Now) const
{
    for (const Effect &One : Live) {
        switch (One.Kind) {
        case EffectKind::Shell:   ShellFine(One, Out, Now); break;
        case EffectKind::Splash:  SplashFine(One, Out, Now); break;
        case EffectKind::Blast:   BlastFine(One, Out, Now); break;
        case EffectKind::Bubbles: BubblesFine(One, Out, Now); break;
        case EffectKind::Salute:  SaluteFine(One, Out, Now); break;
        }
    }
}

}  // namespace seabattle
