/* coin.cpp — the coin's picture, pixel by pixel, each worked out four times
 * over so that its edge is not a stair. */

#include "coin.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "painter/core/color.h"

#include "emblem.h"

namespace coinflip {

namespace {

namespace tui  = painter::tui;
namespace core = painter::core;

// The light, a unit vector: from the upper left, toward the viewer.
constexpr double LightX = -0.5;
constexpr double LightY = 0.6;
constexpr double LightZ = 0.62;
// The face's border, the raised ring inside it, and where the relief ends.
constexpr double BorderFrom = 0.93;
constexpr double RingFrom   = 0.83;
constexpr double ReliefTo   = 0.76;
// The reeding of the rim: so many grooves round it.
constexpr double Grooves = 56.0;

struct Normal {
    double X;
    double Y;
    double Z;
};

// What lies at one point of the picture: how far toward the viewer, and its color.
struct Seen {
    double       Depth;
    core::Color  Ink;
};

class Sampler {
public:
    Sampler(const Look &Shown, double Turn) : Shown(Shown), Cos(std::cos(Turn)), Sin(std::sin(Turn))
    {
        const double Length = std::sqrt(LightX * LightX + LightY * LightY + LightZ * LightZ);
        Light               = {LightX / Length, LightY / Length, LightZ / Length};
        const double Half   = std::sqrt((Light.X * Light.X) + (Light.Y * Light.Y) + (Light.Z + 1.0) * (Light.Z + 1.0));
        Halfway             = {Light.X / Half, Light.Y / Half, (Light.Z + 1.0) / Half};
    }

    // The nearest point of the coin at (U, V), in radii from its centre, up
    // being positive.
    std::optional<Seen> At(double U, double V) const
    {
        std::optional<Seen> Best = Face(U, V);
        if (const std::optional<Seen> Rim = Edge(U, V); Rim && (!Best || Rim->Depth > Best->Depth)) Best = Rim;
        return Best;
    }

private:
    using Color = core::Color;

    // The face turned toward the viewer: heads while the coin's front is up.
    std::optional<Seen> Face(double U, double V) const
    {
        if (std::fabs(Cos) < 1e-6) return std::nullopt;
        const bool   Heads = Cos > 0.0;
        const double W     = Heads ? HalfThickness : -HalfThickness;
        const double B     = (V + W * Sin) / Cos;
        if (U * U + B * B > 1.0) return std::nullopt;
        const Normal Facing = Heads ? Normal{0.0, -Sin, Cos} : Normal{0.0, Sin, -Cos};
        const double Across = U;
        const double Up     = Heads ? B : -B;
        return Seen{B * Sin + W * Cos, Lit(FaceBase(Across, Up, Heads), Facing)};
    }

    // The rim: the nearest of the two points of the cylinder over U.
    std::optional<Seen> Edge(double U, double V) const
    {
        if (std::fabs(Sin) < 1e-6 || std::fabs(U) > 1.0) return std::nullopt;
        const double Round = std::sqrt(1.0 - U * U);
        std::optional<Seen> Best;
        for (const double B : {Round, -Round}) {
            const double W = (B * Cos - V) / Sin;
            if (std::fabs(W) > HalfThickness) continue;
            const double Depth = B * Sin + W * Cos;
            if (Best && Depth <= Best->Depth) continue;
            const Normal Outward{U, B * Cos, B * Sin};
            Best = Seen{Depth, Lit(RimBase(U, B), Outward)};
        }
        return Best;
    }

    Color FaceBase(double Across, double Up, bool Heads) const
    {
        const double Radius = std::sqrt(Across * Across + Up * Up);
        if (Radius >= BorderFrom) return Shown.Metal;
        if (Radius >= RingFrom) return Color::Between(Shown.Metal, core::Colors::White, 70);
        const bool Raised =
            Radius <= ReliefTo && (Heads ? HeadsRaised(Across, Up) : TailsRaised(Across, Up));
        return Raised ? Color::Between(Shown.Metal, core::Colors::White, 60) : Color::Between(Shown.Metal, Shown.Edge, 85);
    }

    Color RimBase(double U, double B) const
    {
        const Color Plain = Color::Between(Shown.Edge, Shown.Metal, 170);
        return std::sin(std::atan2(B, U) * Grooves) > 0.25 ? Color::Between(Plain, Shown.Edge, 110) : Plain;
    }

    // The color under the light: dark where the surface turns from it, a shine
    // where it faces the half-way between the light and the viewer.
    Color Lit(Color Base, Normal Surface) const
    {
        const double Length = std::sqrt(Surface.X * Surface.X + Surface.Y * Surface.Y + Surface.Z * Surface.Z);
        const Normal Unit{Surface.X / Length, Surface.Y / Length, Surface.Z / Length};
        const double Diffuse = std::max(0.0, Unit.X * Light.X + Unit.Y * Light.Y + Unit.Z * Light.Z);
        double       Shine   = std::max(0.0, Unit.X * Halfway.X + Unit.Y * Halfway.Y + Unit.Z * Halfway.Z);
        for (int Square = 0; Square < 5; Square++) Shine *= Shine;
        const Color Shaded = Color::Between(Shown.Edge, Base, static_cast<std::uint8_t>(70.0 + 185.0 * Diffuse));
        return Color::Between(Shaded, core::Colors::White, static_cast<std::uint8_t>(std::min(1.0, Shine * 0.9) * 255.0));
    }

    const Look &Shown;
    double      Cos;
    double      Sin;
    Normal      Light;
    Normal      Halfway;
};

}  // namespace

double Reach(double Turn)
{
    return std::fabs(std::cos(Turn)) + HalfThickness * std::fabs(std::sin(Turn));
}

void PaintCoin(tui::Frame &Out, const Look &Shown, double X, double Y, double Radius, double Turn)
{
    const Sampler Sample(Shown, Turn);
    const double  Height = Radius * Reach(Turn);
    const int     Left   = static_cast<int>(std::floor(X - Radius)) - 1;
    const int     Right  = static_cast<int>(std::ceil(X + Radius)) + 1;
    const int     Top    = static_cast<int>(std::floor(Y - Height)) - 1;
    const int     Bottom = static_cast<int>(std::ceil(Y + Height)) + 1;
    constexpr double Offsets[2] = {0.25, 0.75};

    for (int Row = Top; Row <= Bottom; Row++) {
        for (int Column = Left; Column <= Right; Column++) {
            int Red = 0, Green = 0, Blue = 0, Covered = 0;
            for (const double Down : Offsets) {
                for (const double Across : Offsets) {
                    const double U = (Column + Across - X) / Radius;
                    const double V = (Y - (Row + Down)) / Radius;
                    const std::optional<Seen> Hit = Sample.At(U, V);
                    if (!Hit) continue;
                    Red += Hit->Ink.Red();
                    Green += Hit->Ink.Green();
                    Blue += Hit->Ink.Blue();
                    Covered++;
                }
            }
            if (Covered == 0) continue;
            const core::Color Ink = core::Color::Rgb(static_cast<std::uint8_t>(Red / Covered),
                                                     static_cast<std::uint8_t>(Green / Covered),
                                                     static_cast<std::uint8_t>(Blue / Covered));
            if (Covered == 4) Out.PutPixel({Column, Row}, Ink);
            else Out.BlendPixel({Column, Row}, Ink, static_cast<std::uint8_t>(Covered * 255 / 4));
        }
    }
}

}  // namespace coinflip
