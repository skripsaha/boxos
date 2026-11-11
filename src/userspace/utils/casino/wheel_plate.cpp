/* wheel_plate.cpp — the picture of the wheel (wheel_plate.h). */

#include "wheel_plate.h"

#include <algorithm>
#include <cmath>

#include "common.h"
#include "trig.h"
#include "wheel.h"

namespace casino {

namespace {

const Color Steel = Color::Rgb(0x4A, 0x4C, 0x52);

// The wheel's rings, as fractions of its radius from the middle out.
constexpr float HubEnd      = 0.14f;
constexpr float TurretEnd   = 0.34f;
constexpr float PocketsFrom = 0.38f;
constexpr float PocketsTo   = 0.64f;
constexpr float ConeTo      = 0.80f;
constexpr float TrackTo     = 0.92f;
constexpr float PocketWide  = TwoPi / Pockets;
constexpr int   Depths      = 8;

}  // namespace

void WheelPlate::Build(const Look &Shown, float Across, float Down, float Radius)
{
    Grid = Radius <= 20.0f ? 2 : 1;
    Pixels.clear();
    Samples.clear();
    for (int Which = 0; Which < 3; Which++)
        for (int Depth = 0; Depth < Depths; Depth++)
            Shaded[Which][Depth] = Shade(HueColor(Shown, static_cast<Hue>(Which)), 0.9f + 0.2f * static_cast<float>(Depth) / (Depths - 1));
    for (int Slot = 0; Slot < Pockets; Slot++) Hues[Slot] = static_cast<std::uint8_t>(HueOf(NumberAt(Slot)));

    const std::int32_t Left = static_cast<std::int32_t>(Across - Radius - 1), Top = static_cast<std::int32_t>(Down - Radius - 1);
    const std::int32_t Right = static_cast<std::int32_t>(Across + Radius + 2), Bottom = static_cast<std::int32_t>(Down + Radius + 2);
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        for (std::int32_t Col = Left; Col < Right; Col++) {
            Sample Batch[4];
            bool   Any = false;
            for (int Sub = 0; Sub < Grid * Grid; Sub++) {
                const float Dx = static_cast<float>(Col) + (static_cast<float>(Sub % Grid) + 0.5f) / Grid - Across;
                const float Dy = static_cast<float>(Row) + (static_cast<float>(Sub / Grid) + 0.5f) / Grid - Down;
                Sample     &One = Batch[Sub];
                One.Far         = std::sqrt(Dx * Dx + Dy * Dy);
                if (One.Far >= Radius) continue;
                Any             = true;
                const float Near  = One.Far / Radius;
                const float Light = One.Far > 0 ? (-Dx - Dy) / (1.4142f * One.Far) : 0.0f;
                One.Bearing       = Bearing(Dx, Dy);
                if (One.Bearing < 0) One.Bearing += TwoPi;
                One.Kind = Part::Still;
                if (Near < HubEnd) {
                    One.Fixed = Shade(Shown.Gold, 0.8f + 0.5f * (1.0f - Near / HubEnd) * (0.5f + 0.5f * Light));
                } else if (Near < TurretEnd) {
                    One.Kind  = Part::Spokes;
                    One.Fixed = Shade(Shown.Wheel, 0.55f);
                    One.Alt   = Shade(Shown.Gold, 0.95f);
                } else if (Near < PocketsFrom) {
                    One.Fixed = Shade(Shown.Gold, 0.85f + 0.2f * Light);
                } else if (Near < PocketsTo) {
                    One.Kind  = Part::Pockets;
                    One.Alt   = Shade(Shown.Gold, 0.9f);
                    One.Depth = static_cast<std::uint8_t>((Depths - 1) * (Near - PocketsFrom) / (PocketsTo - PocketsFrom));
                } else if (Near < PocketsTo + 0.04f) {
                    One.Fixed = Shade(Shown.Gold, 0.75f);
                } else if (Near < ConeTo) {
                    One.Fixed = Shade(Shown.Wheel, 1.0f - 0.25f * (ConeTo - Near) / (ConeTo - PocketsTo) + 0.1f * Light);
                } else if (Near < TrackTo) {
                    One.Fixed = Shade(Steel, 0.8f + 0.5f * (Near - ConeTo) / (TrackTo - ConeTo) + 0.25f * Light);
                } else {
                    One.Fixed = Shade(Shown.Wheel, 1.0f + 0.3f * Light - 0.3f * (Near - TrackTo) / (1.0f - TrackTo));
                }
            }
            if (!Any) continue;
            Pixels.push_back(Pixel{Col, Row});
            Samples.insert(Samples.end(), Batch, Batch + Grid * Grid);
        }
    }
}

Color WheelPlate::Seen(const Sample &At, float Angle, int Glow, float Pulse) const
{
    if (At.Kind == Part::Still) return At.Fixed;
    const float Turn = At.Bearing - Angle + TwoPi;
    if (At.Kind == Part::Spokes) {
        const float Quarter = Turn * (2.0f / Pi);
        const float Fraction = Quarter - static_cast<float>(static_cast<int>(Quarter));
        return std::min(Fraction, 1.0f - Fraction) * HalfPi * At.Far < 0.8f ? At.Alt : At.Fixed;
    }
    const float Place    = Turn * (1.0f / PocketWide) + 0.5f;
    const int   Whole    = static_cast<int>(Place);
    const float Fraction = Place - static_cast<float>(Whole);
    if (std::min(Fraction, 1.0f - Fraction) * PocketWide * At.Far < 0.45f && PocketWide * At.Far >= 2.0f) return At.Alt;
    const int Slot = Whole % Pockets;
    const Color Pocket = Shaded[Hues[Slot]][At.Depth];
    return Slot == Glow ? Shade(Pocket, 1.0f + 0.7f * Pulse) : Pocket;
}

void WheelPlate::Paint(Canvas &Draw, const Look &Shown, std::uint32_t Revision, float Across, float Down, float Radius, float Angle,
                       int Glow, float Pulse)
{
    if (Key[0] != Across || Key[1] != Down || Key[2] != Radius || Version != Revision) {
        Build(Shown, Across, Down, Radius);
        Key[0]  = Across;
        Key[1]  = Down;
        Key[2]  = Radius;
        Version = Revision;
    }
    const float Turned = Angle - TwoPi * std::floor(Angle / TwoPi);
    const int   Each   = Grid * Grid;
    for (std::size_t At = 0; At < Pixels.size(); At++) {
        std::int32_t Sum[3] = {0, 0, 0};
        int          Inside = 0;
        for (int Sub = 0; Sub < Each; Sub++) {
            const Sample &One = Samples[At * static_cast<std::size_t>(Each) + static_cast<std::size_t>(Sub)];
            if (One.Kind == Part::Outside) continue;
            const Color Hit = Seen(One, Turned, Glow, Pulse);
            Sum[0] += Hit.Red();
            Sum[1] += Hit.Green();
            Sum[2] += Hit.Blue();
            Inside++;
        }
        Draw.Plot(Pixels[At].Col, Pixels[At].Row,
                  Color::Rgb(static_cast<std::uint8_t>(Sum[0] / Inside), static_cast<std::uint8_t>(Sum[1] / Inside),
                             static_cast<std::uint8_t>(Sum[2] / Inside)),
                  static_cast<float>(Inside) / static_cast<float>(Each));
    }
}

}  // namespace casino
