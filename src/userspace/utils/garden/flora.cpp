/* flora.cpp — a plant, pixel by pixel. */

#include "flora.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace garden {

namespace {

using painter::core::Colors::White;

constexpr std::int32_t PulseFrames = 24;

std::uint8_t Part(double Share) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(Share, 0.0, 1.0) * 255.0);
}

Ink Between(Ink A, Ink B, double Share) noexcept
{
    return Ink::Mix(A, B, Part(Share));
}

std::int32_t Round(double Value) noexcept
{
    return static_cast<std::int32_t>(std::lround(Value));
}

enum class Flower : std::uint8_t { Daisy, Tulip, Bell };

class Plot {
public:
    Plot(const Pen &Draw, const Palette &Colors, const PlantPose &Pose, double Time)
        : Draw(Draw), Colors(Colors), Pose(Pose), Ground(Draw.Of().Ground), Weak(1.0 - Pose.Fresh),
          Side((Pose.Seed & 1u) ? 1 : -1), Kind(static_cast<Flower>((Pose.Seed >> 1) % 3)),
          Petal(Colors.Bloom[(Pose.Seed >> 5) % 3]),
          Wind(0.6 * std::sin(Time * 1.3 + Pose.X * 0.21) + 0.4 * std::sin(Time * 2.9 + Pose.X * 0.5)),
          Amp(std::min(3.0, 0.5 + Pose.Height / 10.0)), Droop(std::pow(Weak, 1.4) * Pose.Height * 0.4),
          Time(Time)
    {
    }

    Crest Grow() const
    {
        const Crest Top = Pose.Reached == Stage::Tree ? Tree() : Herb();
        if (Pose.Pulse > 0) Sparkle(Top);
        return Top;
    }

private:
    // Where the plant is Along pixels up from its root: swayed by the wind,
    // and, the weaker it is, bent over at the top.
    Crest Spot(double Along) const
    {
        const double Share = Pose.Height > 0 ? Along / Pose.Height : 0.0;
        const double Lean  = Share * Share * Share;
        return Crest{Round(Pose.X + Wind * Amp * Share * Share + Side * Droop * Lean),
                     Round(Ground - Along + Droop * 0.45 * Lean)};
    }

    Ink Foliage(double Shade) const
    {
        return Between(Between(Colors.Leaf, Colors.Shade, Shade), Colors.Wilted, Weak);
    }

    // A leaf of Length pixels from From, to Side: up while the plant is well,
    // hanging when it is not.
    void Leaf(Crest From, std::int32_t Toward, std::int32_t Length) const
    {
        const std::int32_t Thick = Pose.Height >= 18 ? 2 : 1;
        for (std::int32_t Step = 1; Step <= Length; Step++) {
            const std::int32_t Y = From.Y + (Weak < 0.5 ? -(Step / 2) : (Step + 1) / 2);
            for (std::int32_t Row = 0; Row < Thick; Row++) Draw.Dot(From.X + Toward * Step, Y + Row, Foliage(0.6 * Step / Length));
        }
    }

    bool Leafy(std::uint32_t Which) const
    {
        return Unit(Scatter(Pose.Seed + Which * 17u)) < 0.3 + Pose.Fresh * 0.85;
    }

    void Stem() const
    {
        const Ink Wood = Between(Colors.Stem, Colors.Wilted, Weak * 0.8);
        for (double Along = 0; Along <= Pose.Height; Along += 0.5) {
            const Crest At = Spot(Along);
            Draw.Dot(At.X, At.Y, Wood);
            if (Pose.Reached >= Stage::Young && Along < Pose.Height * 0.7) Draw.Dot(At.X + 1, At.Y, Wood);
        }
    }

    void Leaves() const
    {
        const bool Pairs = Pose.Reached <= Stage::Seedling;
        const int  Count = Pose.Reached == Stage::Sprout ? 1 : Pose.Reached == Stage::Seedling ? 2
                         : Pose.Reached == Stage::Young ? 4 : Pose.Reached == Stage::Blooming ? 6 : 8;
        const auto Length = static_cast<std::int32_t>(std::clamp(Pose.Height / 6.0, 2.0, 5.0));
        for (int Which = 0; Which < Count; Which++) {
            if (!Leafy(static_cast<std::uint32_t>(Which))) continue;
            const double Along = Pairs && Which == Count - 1 ? Pose.Height
                               : 2.0 + (Pose.Height - 4.0) * (Which + 1) / (Count + (Pairs ? 0 : 1));
            const Crest At = Spot(Along);
            if (Pairs) {
                Leaf(At, -1, Length);
                Leaf(At, 1, Length);
            } else {
                Leaf(At, Which % 2 ? 1 : -1, Length);
            }
        }
    }

    void Daisy(Crest At, std::int32_t Reach) const
    {
        for (std::int32_t Dy = -Reach; Dy <= Reach; Dy++)
            for (std::int32_t Dx = -Reach; Dx <= Reach; Dx++)
                if (Dx * Dx + Dy * Dy <= Reach * Reach) Draw.Dot(At.X + Dx, At.Y + Dy, Petal);
        Draw.Dot(At.X, At.Y, Colors.Bloom[((Pose.Seed >> 5) + 1) % 3]);
    }

    void Tulip(Crest At, std::int32_t Reach) const
    {
        const Ink Inner = Between(Petal, Colors.Stem, 0.25);
        for (std::int32_t Dy = -1; Dy <= 1; Dy++)
            for (std::int32_t Dx = -Reach + 1; Dx <= Reach - 1; Dx++) Draw.Dot(At.X + Dx, At.Y + Dy, Dx == 0 ? Inner : Petal);
        Draw.Dot(At.X - Reach + 1, At.Y - 2, Petal);
        Draw.Dot(At.X + Reach - 1, At.Y - 2, Petal);
    }

    void Bell(Crest At, std::int32_t Reach) const
    {
        for (std::int32_t Dx = -1; Dx <= 1; Dx++) Draw.Dot(At.X + Dx, At.Y, Petal);
        for (std::int32_t Dx = -Reach + 1; Dx <= Reach - 1; Dx++) Draw.Dot(At.X + Dx, At.Y + 1, Petal);
        Draw.Dot(At.X, At.Y + 2, Between(Petal, White, 0.5));
    }

    // The flower of a plant in bloom; the wilted head of one that is not, and
    // its petals on the ground.
    void Head(Crest At) const
    {
        const std::int32_t Reach = Pose.Reached == Stage::Grown ? 3 : 2;
        if (Pose.Fresh >= 0.5) {
            if (Kind == Flower::Daisy) Daisy(At, Reach);
            else if (Kind == Flower::Tulip) Tulip(At, Reach + 1);
            else Bell(At, Reach + 1);
            return;
        }
        Draw.Dot(At.X, At.Y, Colors.Wilted);
        Draw.Dot(At.X + Side, At.Y + 1, Colors.Wilted);
        for (std::int32_t Fell = 0; Fell < 3; Fell++)
            Draw.Veil(Pose.X + (Fell - 1) * 2 + static_cast<std::int32_t>(Pose.Seed % 3), Ground - 1, Petal, 170);
    }

    Crest Herb() const
    {
        Stem();
        Leaves();
        const Crest Top = Spot(Pose.Height);
        if (Pose.Reached >= Stage::Blooming) Head(Top);
        else if (Pose.Reached == Stage::Young && Pose.Fresh >= 0.4) {
            Draw.Dot(Top.X, Top.Y, Between(Petal, Colors.Leaf, 0.4));
            Draw.Dot(Top.X, Top.Y - 1, Between(Petal, Colors.Leaf, 0.2));
        }
        return Top;
    }

    // A trunk and a crown of leaves, lacier the weaker it is, with fruit in it.
    Crest Tree() const
    {
        const double Trunk = Pose.Height * 0.5;
        const Ink    Bark  = Between(Colors.Soil, Colors.Wilted, 0.5);
        const double Sway  = Wind * 1.5;
        for (double Along = 0; Along <= Trunk; Along += 0.5) {
            const std::int32_t X = Pose.X + Round(Sway * 0.4 * Along / Trunk);
            Draw.Dot(X, Ground - Round(Along), Bark);
            Draw.Dot(X + 1, Ground - Round(Along), Bark);
        }
        const std::int32_t Reach = 4 + static_cast<std::int32_t>(Pose.Height / 12.0);
        const std::int32_t Tall  = std::max(3, Round(Pose.Height * 0.28));
        const std::int32_t CenterX = Pose.X + Round(Sway);
        const std::int32_t CenterY = Ground - Round(Trunk) - Tall / 2;
        const std::uint32_t Density = static_cast<std::uint32_t>(45 + 50 * Pose.Fresh);
        for (std::int32_t Dy = -Tall; Dy <= Tall; Dy++)
            for (std::int32_t Dx = -Reach; Dx <= Reach; Dx++) {
                if (Dx * Dx * Tall * Tall + Dy * Dy * Reach * Reach > Reach * Reach * Tall * Tall) continue;
                if (Scatter(Pose.Seed ^ static_cast<std::uint32_t>(Dx * 131 + Dy * 7919)) % 100 >= Density) continue;
                Draw.Dot(CenterX + Dx, CenterY + Dy, Foliage(0.7 * (Dy + Tall) / (2.0 * Tall)));
            }
        if (Pose.Fresh >= 0.35)
            for (std::uint32_t Fruit = 0; Fruit < 4; Fruit++) {
                const double Angle = Unit(Scatter(Pose.Seed + Fruit * 29u)) * 2.0 * std::numbers::pi;
                const double Within = 0.35 + 0.5 * Unit(Scatter(Pose.Seed + Fruit * 53u));
                Draw.Dot(CenterX + Round(std::cos(Angle) * Reach * Within), CenterY + Round(std::sin(Angle) * Tall * Within),
                         Colors.Fruit);
            }
        else
            for (std::int32_t Fallen = 0; Fallen < 4; Fallen++)
                Draw.Dot(Pose.X + (Fallen - 2) * 2 + Side, Ground - 1, Colors.Wilted);
        return Crest{CenterX, CenterY - Tall};
    }

    // Four sparks that circle the top while a start just heard is new.
    void Sparkle(Crest Top) const
    {
        const double Out = 3.0 + (PulseFrames - Pose.Pulse) / 5.0;
        for (int Spark = 0; Spark < 4; Spark++) {
            const double Angle = Time * 6.0 + Spark * std::numbers::pi / 2.0;
            Draw.Veil(Top.X + Round(std::cos(Angle) * Out), Top.Y + Round(std::sin(Angle) * Out), White,
                      Part(Pose.Pulse / static_cast<double>(PulseFrames)));
        }
    }

    const Pen        &Draw;
    const Palette    &Colors;
    const PlantPose  &Pose;
    std::int32_t      Ground;
    double            Weak;       // 0 well, 1 as wilted as it goes
    std::int32_t      Side;       // the way it leans when it is weak
    Flower            Kind;
    Ink               Petal;
    double            Wind;
    double            Amp;
    double            Droop;
    double            Time;
};

}  // namespace

Crest PaintPlant(const Pen &Draw, const Palette &Colors, const PlantPose &Pose, double Time)
{
    return Plot(Draw, Colors, Pose, Time).Grow();
}

}  // namespace garden
