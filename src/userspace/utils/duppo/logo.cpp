#include "logo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace duppo {

namespace {

constexpr Color LightDefault    = Color::Rgb(0xE9, 0x78, 0xA0);
constexpr Color DeepDefault     = Color::Rgb(0x81, 0x26, 0x5C);
constexpr Color LipDefault      = Color::Rgb(0xE8, 0x66, 0x96);
constexpr Color RingDefault     = Color::Rgb(0xFF, 0xE2, 0xF0);
constexpr Color RingEdgeDefault = Color::Rgb(0x8B, 0x60, 0x77);

/* How much white the pupil's glow and its rim are mixed with: #3B and #29 over black. */
constexpr std::uint8_t GlowShare   = 0x3B;
constexpr std::uint8_t EdgeShare   = 0x29;
constexpr std::uint8_t HazeShare   = 205;

std::uint8_t Moved(std::uint8_t Base, std::uint8_t Was, std::uint8_t Now)
{
    return static_cast<std::uint8_t>(std::clamp(Base + Now - Was, 0, 255));
}

/* A colour moved the way a default colour was moved to the one set. */
Color Followed(Color Base, Color Was, Color Now)
{
    return Color::Rgb(Moved(Base.Red(), Was.Red(), Now.Red()), Moved(Base.Green(), Was.Green(), Now.Green()),
                      Moved(Base.Blue(), Was.Blue(), Now.Blue()));
}

/* A radial gradient as the SVG gives it: the circle (Center, Radius) lives in
 * a space that the matrix (A B C D E F, as SVG's matrix()) maps into the
 * disc's own, so a point is taken back through it and measured from the centre. */
struct Gradient {
    float A, B, C, D, E, F;
    Vec   Center;
    float Radius;

    float At(Vec Point) const
    {
        const float X     = Point.X - E, Y = Point.Y - F;
        const float Det   = A * D - B * C;
        const float InX   = (D * X - C * Y) / Det - Center.X;
        const float InY   = (A * Y - B * X) / Det - Center.Y;
        return std::sqrt(InX * InX + InY * InY) / Radius;
    }
};

struct Stop {
    float At;
    Color Ink;
};

Color Ramp(float Along, Stop First, Stop Second)
{
    if (Along <= First.At) return First.Ink;
    if (Along >= Second.At) return Second.Ink;
    float Share = (Along - First.At) / (Second.At - First.At);
    return Color::Mix(First.Ink, Second.Ink, static_cast<std::uint8_t>(Share * 255.0f + 0.5f));
}

Color Ramp(float Along, Stop First, Stop Second, Stop Third)
{
    return Along <= Second.At ? Ramp(Along, First, Second) : Ramp(Along, Second, Third);
}

constexpr Gradient BodyGradient{1.143646f, -0.09094687f, 0.09189171f, 1.1555275f, -124.59179f, -94.515235f, {128.0959f, 112.31412f}, 16.922415f};
constexpr Gradient SocketGradient{0.54407561f, 0.0f, 0.0f, 0.54407561f, -22.114865f, -36.849767f, {103.13065f, 115.31994f}, 21.151943f};
constexpr Gradient WhiteGradient{0.57626908f, -0.00182151f, 0.00186949f, 0.59144541f, -25.591621f, -42.04433f, {101.40121f, 113.55885f}, 18.404936f};
constexpr Gradient PupilGradient{1.1021106f, 0.92894668f, -1.0140942f, 1.2031302f, 7.747513f, -225.95277f, {124.25608f, 110.82202f}, 7.3780317f};
constexpr Gradient MoonGradients[Logo::MoonCount] = {
    {0.34233561f, -0.02722377f, 0.02750658f, 0.3458922f, -2.6037641f, 5.9093724f, {128.0959f, 112.31412f}, 16.922415f},
    {0.23769016f, -0.01890198f, 0.01909834f, 0.24015957f, -16.083896f, -10.419087f, {128.0959f, 112.31412f}, 16.922415f},
    {0.16609062f, -0.01320812f, 0.01334534f, 0.16781617f, 32.475734f, 5.2852998f, {128.0959f, 112.31412f}, 16.922415f},
};

}  // namespace

Palette Palette::Make(Color Body, Color RingSet, Color Pupil)
{
    const Color Deep = Followed(DeepDefault, LightDefault, Body);
    return Palette{.Light     = Body,
                   .Deep      = Deep,
                   .Lip       = Followed(LipDefault, LightDefault, Body),
                   .Ring      = RingSet,
                   .RingEdge  = Followed(RingEdgeDefault, RingDefault, RingSet),
                   .Glow      = Color::Mix(Pupil, painter::core::Colors::White, GlowShare),
                   .Core      = Pupil,
                   .PupilEdge = Color::Mix(Pupil, painter::core::Colors::White, EdgeShare),
                   .Haze      = Color::Mix(Deep, painter::core::Colors::Black, HazeShare)};
}

namespace Logo {

Color BodyColor(const Palette &Hues, Vec At)
{
    return Ramp(BodyGradient.At(At), {0.51822966f, Hues.Light}, {0.9999f, Hues.Deep});
}

Color SocketColor(const Palette &Hues, Vec At)
{
    return Ramp(SocketGradient.At(At), {0.87368202f, Hues.Deep}, {1.0f, Hues.Lip});
}

Color WhiteColor(const Palette &Hues, Vec At)
{
    return Ramp(WhiteGradient.At(At), {0.7325418f, Hues.Ring}, {0.98360658f, Hues.RingEdge});
}

Color PupilColor(const Palette &Hues, Vec At)
{
    return Ramp(PupilGradient.At(At), {0.03465074f, Hues.Glow}, {0.8121683f, Hues.Core}, {0.9999f, Hues.PupilEdge});
}

Color MoonColor(const Palette &Hues, int Which, Vec At)
{
    return Ramp(MoonGradients[Which].At(At), {0.0f, Hues.Light}, {0.9297424f, Hues.Deep});
}

}  // namespace Logo

}  // namespace duppo
