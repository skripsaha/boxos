// paint.cpp — the glow of the trails, the bodies, the flares and the aim.

#include "paint.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbits {

namespace {

constexpr double ViewHalf     = 1.5;      // how much of the sky is seen above the middle at zoom 1
constexpr double FadeTo       = 1.0 / 48; // what a trail is left with when its time is out
constexpr float  GlowFloor    = 0.4f;     // below this a trail is gone, not a number ever smaller
constexpr double BurstSeconds = 0.8;
constexpr double FullHeight   = 100.0;   // pixels high from which the glows are as wide as they are drawn
constexpr int    LeastCells   = 1;

const Color Dust = Color::Rgb(0xC8, 0xD4, 0xFF);
const Color Hot  = Color::Rgb(0xFF, 0xFF, 0xFF);

int Floor(double Value)
{
    return static_cast<int>(std::floor(Value));
}

std::uint32_t Hash(int X, int Y)
{
    std::uint32_t Mixed = static_cast<std::uint32_t>(X) * 0x9E3779B1u ^ static_cast<std::uint32_t>(Y) * 0x85EBCA6Bu;
    Mixed ^= Mixed >> 15;
    Mixed *= 0x2C1B3C6Du;
    return Mixed ^ (Mixed >> 12);
}

std::uint8_t Byte(double Value)
{
    return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0);
}

Color InkOf(const Look &Shown, const Body &One)
{
    return One.IsStar() ? StarColor(Shown, One.Tone) : BodyColor(Shown, One.Tone);
}

}  // namespace

void Canvas::Fit(painter::core::Size Cells)
{
    Wide  = std::max(Cells.Width, LeastCells);
    High  = std::max(Cells.Height, LeastCells) * 2;
    Scale = High / (2.0 * ViewHalf) * ZoomFactor;
    Reach = std::clamp(High / FullHeight, 0.3, 1.0);
    Pixels.assign(static_cast<std::size_t>(Wide) * High, Color());
    Clear();
}

void Canvas::Zoom(double To)
{
    ZoomFactor = To;
    Scale      = High / (2.0 * ViewHalf) * ZoomFactor;
    Clear();
}

void Canvas::Clear()
{
    Glows.assign(static_cast<std::size_t>(Wide) * High, Glow{});
}

Canvas::Spot Canvas::Where(Vec At) const
{
    return {Wide / 2.0 + At.X * Scale, High / 2.0 - At.Y * Scale};
}

void Canvas::Age(double Seconds, const Look &Shown)
{
    if (Seconds <= 0.0) return;
    Time += Seconds;
    const float Fading = static_cast<float>(std::pow(FadeTo, Seconds / std::max<double>(Shown.TrailSeconds, 1.0)));
    for (Glow &One : Glows) {
        One.Red   = One.Red * Fading < GlowFloor ? 0.0f : One.Red * Fading;
        One.Green = One.Green * Fading < GlowFloor ? 0.0f : One.Green * Fading;
        One.Blue  = One.Blue * Fading < GlowFloor ? 0.0f : One.Blue * Fading;
    }
    for (Burst &One : Bursts) One.Age += Seconds / BurstSeconds;
    std::erase_if(Bursts, [](const Burst &One) { return One.Age >= 1.0; });
}

void Canvas::Lay(int X, int Y, Color Ink, double Strength)
{
    if (X < 0 || Y < 0 || X >= Wide || Y >= High) return;
    Glow &One = Glows[static_cast<std::size_t>(Y) * Wide + X];
    One.Red   = std::max(One.Red, static_cast<float>(Ink.Red() * Strength));
    One.Green = std::max(One.Green, static_cast<float>(Ink.Green() * Strength));
    One.Blue  = std::max(One.Blue, static_cast<float>(Ink.Blue() * Strength));
}

void Canvas::Trail(Body &One, const Look &Shown)
{
    const Spot   From  = Where(One.Drawn);
    const Spot   To    = Where(One.At);
    const double Along = To.X - From.X, Down = To.Y - From.Y;
    const int    Steps = static_cast<int>(std::ceil(std::max(std::fabs(Along), std::fabs(Down))));
    if (Steps < 1) return;
    if (Steps <= 4 * Wide) {
        const Color  Ink      = InkOf(Shown, One);
        const double Strength = One.IsStar() ? 0.55 : 0.85;
        for (int Step = 0; Step <= Steps; Step++) {
            const double Part = static_cast<double>(Step) / Steps;
            Lay(Floor(From.X + Along * Part), Floor(From.Y + Down * Part), Ink, Strength);
        }
    }
    One.Drawn = One.At;
}

void Canvas::Flare(const Collision &Hit, const Look &Shown)
{
    const double Size = (3.0 + 2.5 * std::cbrt(Hit.Mass)) * std::max(0.8, High / 110.0);
    const Color  Ink  = Hit.Star ? StarColor(Shown, Hit.Tone) : BodyColor(Shown, Hit.Tone);
    Bursts.push_back(Burst{Hit.At, 0.0, std::min(Size, 28.0), Ink});
}

void Canvas::Blend(int X, int Y, Color Ink, double Weight)
{
    if (X < 0 || Y < 0 || X >= Wide || Y >= High) return;
    Color &Under = Pixels[static_cast<std::size_t>(Y) * Wide + X];
    Under        = Color::Mix(Under, Ink, Byte(Weight));
}

void Canvas::Disc(Spot At, double Radius, Color Ink, double Weight)
{
    const int Left = std::max(Floor(At.X - Radius - 1), 0), Right = std::min(Floor(At.X + Radius + 1), Wide - 1);
    const int Top = std::max(Floor(At.Y - Radius - 1), 0), Bottom = std::min(Floor(At.Y + Radius + 1), High - 1);
    for (int Y = Top; Y <= Bottom; Y++) {
        for (int X = Left; X <= Right; X++) {
            const double Away  = std::hypot(X + 0.5 - At.X, Y + 0.5 - At.Y);
            const double Cover = std::clamp(Radius + 0.5 - Away, 0.0, 1.0);
            if (Cover > 0.0) Blend(X, Y, Ink, Cover * Weight);
        }
    }
}

void Canvas::Halo(Spot At, double Inner, double Outer, Color Ink, double Strength)
{
    const int Left = std::max(Floor(At.X - Outer), 0), Right = std::min(Floor(At.X + Outer), Wide - 1);
    const int Top = std::max(Floor(At.Y - Outer), 0), Bottom = std::min(Floor(At.Y + Outer), High - 1);
    for (int Y = Top; Y <= Bottom; Y++) {
        for (int X = Left; X <= Right; X++) {
            const double Away = std::hypot(X + 0.5 - At.X, Y + 0.5 - At.Y);
            if (Away >= Outer) continue;
            const double Rest = 1.0 - std::max(0.0, (Away - Inner) / (Outer - Inner));
            Blend(X, Y, Ink, Strength * Rest * Rest);
        }
    }
}

// The night: the background, a sparse dust of faint stars that glimmer, and the
// trails laid over it.
void Canvas::Background(const Look &Shown)
{
    const Color Base = Shown.Background;
    for (int Y = 0; Y < High; Y++) {
        for (int X = 0; X < Wide; X++) {
            const std::uint32_t Chance = Hash(X, Y);
            Color               Night  = Base;
            if (((Chance >> 4) & 0x1FF) == 0) {
                const double Glimmer = 0.75 + 0.25 * std::sin(Time * (0.6 + ((Chance >> 24) & 7) * 0.3) + (Chance & 0xFFF));
                Night = Color::Mix(Base, Dust, Byte((0.10 + 0.20 * ((Chance >> 16) & 0xFF) / 255.0) * Glimmer));
            }
            const Glow &Trail = Glows[static_cast<std::size_t>(Y) * Wide + X];
            const auto  Sum   = [](std::uint8_t Under, float Over) {
                return static_cast<std::uint8_t>(std::min(255.0f, Under + Over));
            };
            Pixels[static_cast<std::size_t>(Y) * Wide + X] =
                Color::Rgb(Sum(Night.Red(), Trail.Red), Sum(Night.Green(), Trail.Green), Sum(Night.Blue(), Trail.Blue));
        }
    }
}

void Canvas::PaintStar(const Body &One, const Look &Shown)
{
    const Color  Ink    = StarColor(Shown, One.Tone);
    const Spot   At     = Where(One.At);
    const double Radius = std::max(3.0, One.Radius * Scale) * (1.0 + 0.05 * std::sin(Time * 2.1 + One.Id));
    Halo(At, Radius * 0.5, Radius * (1.0 + 2.4 * Reach), Ink, 0.7);
    Disc(At, Radius, Color::Mix(Ink, Hot, 90), 1.0);
    Disc(At, Radius * 0.55, Color::Mix(Ink, Hot, 200), 0.9);
}

void Canvas::PaintBody(const Body &One, const Look &Shown)
{
    const Color  Ink    = BodyColor(Shown, One.Tone);
    const Spot   At     = Where(One.At);
    const double Radius = std::max(One.Radius * Scale, 0.6 + 0.45 * std::cbrt(One.Mass));
    Halo(At, 0.0, Radius + 2.5 * Reach, Ink, 0.3);
    Disc(At, Radius, Ink, 1.0);
    if (Radius > 1.4) Disc(At, Radius * 0.5, Color::Mix(Ink, Hot, 150), 0.8);
}

void Canvas::PaintBursts()
{
    for (const Burst &One : Bursts) {
        const Spot   At     = Where(One.At);
        const double Radius = One.Size * std::sqrt(One.Age) + 1.0;
        const double Fading = std::pow(1.0 - One.Age, 1.5);
        const Color  Ink    = Color::Mix(Hot, One.Ink, Byte(One.Age));
        const int    Left = std::max(Floor(At.X - Radius - 2), 0), Right = std::min(Floor(At.X + Radius + 2), Wide - 1);
        const int    Top = std::max(Floor(At.Y - Radius - 2), 0), Bottom = std::min(Floor(At.Y + Radius + 2), High - 1);
        for (int Y = Top; Y <= Bottom; Y++) {
            for (int X = Left; X <= Right; X++) {
                const double Away = std::hypot(X + 0.5 - At.X, Y + 0.5 - At.Y);
                const double Ring = std::max(0.0, 1.0 - std::fabs(Away - Radius * 0.9) / 1.6);
                const double Fill = Away < Radius ? 0.55 * (1.0 - Away / Radius) : 0.0;
                Blend(X, Y, Ink, std::max(Ring, Fill) * Fading);
            }
        }
    }
}

// The launcher as a ring with a tick the way the throw goes, and the path as
// dots that thin out with distance, ending in a cross where it ends in a touch.
void Canvas::PaintAim(const Aiming &Aim, const Look &Shown)
{
    if (!Aim.Shown) return;
    const Color Ink = Shown.Aim;
    const Spot  At  = Where(Aim.Launcher);
    for (int Part = 0; Part < 12; Part++) {
        const double Angle = Part * std::numbers::pi / 6.0;
        Blend(Floor(At.X + 3.0 * std::cos(Angle)), Floor(At.Y + 3.0 * std::sin(Angle)), Ink, 0.7);
    }
    for (double Along = 5.0; Along <= 8.0; Along += 1.0)
        Blend(Floor(At.X + Along * std::cos(Aim.Angle)), Floor(At.Y - Along * std::sin(Aim.Angle)), Ink, 0.9);
    const std::size_t Count = Aim.Places.size();
    for (std::size_t Place = 0; Place < Count; Place += 2) {
        const Spot Dot = Where(Aim.Places[Place]);
        Blend(Floor(Dot.X), Floor(Dot.Y), Ink, 0.95 * (1.0 - 0.75 * static_cast<double>(Place) / Count));
    }
    if (!Aim.Ends || Count == 0) return;
    const Spot End = Where(Aim.Places.back());
    for (int Offset = -1; Offset <= 1; Offset++) {
        Blend(Floor(End.X) + Offset, Floor(End.Y) + Offset, Ink, 0.9);
        Blend(Floor(End.X) + Offset, Floor(End.Y) - Offset, Ink, 0.9);
    }
}

void Canvas::Paint(painter::tui::Frame &Out, const Sky &Bodies, const Look &Shown, const Aiming &Aim)
{
    Background(Shown);
    for (const Body &One : Bodies.Bodies)
        if (One.IsStar()) PaintStar(One, Shown);
    for (const Body &One : Bodies.Bodies)
        if (!One.IsStar()) PaintBody(One, Shown);
    PaintBursts();
    PaintAim(Aim, Shown);
    Out.PutPixels(Pixels);
}

}  // namespace orbits
