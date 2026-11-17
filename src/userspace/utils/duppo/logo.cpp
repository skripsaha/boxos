#include "logo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace duppo {

namespace {

constexpr Color LightDefault    = Color::FromRaw(BoxDuppoHues[BOXOS_DUPPO_LIGHT]);
constexpr Color DeepDefault     = Color::FromRaw(BoxDuppoHues[BOXOS_DUPPO_DEEP]);
constexpr Color LipDefault      = Color::FromRaw(BoxDuppoHues[BOXOS_DUPPO_LIP]);
constexpr Color RingDefault     = Color::FromRaw(BoxDuppoHues[BOXOS_DUPPO_RING]);
constexpr Color RingEdgeDefault = Color::FromRaw(BoxDuppoHues[BOXOS_DUPPO_RING_EDGE]);

/* How much white the pupil's glow and its rim are mixed with. */
constexpr std::uint8_t GlowShare   = BOXOS_DUPPO_GLOW_SHARE;
constexpr std::uint8_t EdgeShare   = BOXOS_DUPPO_RIM_SHARE;
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

/* A radial gradient as boxos_mascot.h gives it: the circle (Center, Radius)
 * lives in a space that the matrix (A B C D E F, as SVG's matrix()) maps into
 * the disc's own, so a point is taken back through it and measured from the
 * centre. */
struct Gradient {
    float A, B, C, D, E, F;
    Vec   Center;
    float Radius;

    constexpr explicit Gradient(const BoxDuppoShade &Shade)
        : A(Shade.A), B(Shade.B), C(Shade.C), D(Shade.D), E(Shade.E), F(Shade.F), Center{Shade.CX, Shade.CY},
          Radius(Shade.Radius)
    {
    }

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
    return Color::Between(First.Ink, Second.Ink, static_cast<std::uint8_t>(Share * 255.0f + 0.5f));
}

Color Ramp(float Along, Stop First, Stop Second, Stop Third)
{
    return Along <= Second.At ? Ramp(Along, First, Second) : Ramp(Along, Second, Third);
}

constexpr Gradient BodyGradient{BoxDuppoBodyShade};
constexpr Gradient SocketGradient{BoxDuppoSocketShade};
constexpr Gradient WhiteGradient{BoxDuppoWhiteShade};
constexpr Gradient PupilGradient{BoxDuppoPupilShade};
constexpr Gradient MoonGradients[Logo::MoonCount] = {
    Gradient{BoxDuppoMoonShades[0]}, Gradient{BoxDuppoMoonShades[1]}, Gradient{BoxDuppoMoonShades[2]}};

/* The hues each disc's stops are drawn in are the palette's, the one the
 * person set: they are held here to the ones boxos_mascot.h names. */
static_assert(BoxDuppoBodyStops[0].Hue == BOXOS_DUPPO_LIGHT && BoxDuppoBodyStops[1].Hue == BOXOS_DUPPO_DEEP);
static_assert(BoxDuppoSocketStops[0].Hue == BOXOS_DUPPO_DEEP && BoxDuppoSocketStops[1].Hue == BOXOS_DUPPO_LIP);
static_assert(BoxDuppoWhiteStops[0].Hue == BOXOS_DUPPO_RING && BoxDuppoWhiteStops[1].Hue == BOXOS_DUPPO_RING_EDGE);
static_assert(BoxDuppoPupilStops[0].Hue == BOXOS_DUPPO_GLOW && BoxDuppoPupilStops[1].Hue == BOXOS_DUPPO_CORE &&
              BoxDuppoPupilStops[2].Hue == BOXOS_DUPPO_PUPIL_EDGE);
static_assert(BoxDuppoMoonStops[0].Hue == BOXOS_DUPPO_LIGHT && BoxDuppoMoonStops[1].Hue == BOXOS_DUPPO_DEEP);

}  // namespace

Palette Palette::Make(Color Body, Color RingSet, Color Pupil)
{
    const Color Deep = Followed(DeepDefault, LightDefault, Body);
    return Palette{.Light     = Body,
                   .Deep      = Deep,
                   .Lip       = Followed(LipDefault, LightDefault, Body),
                   .Ring      = RingSet,
                   .RingEdge  = Followed(RingEdgeDefault, RingDefault, RingSet),
                   .Glow      = Color::Between(Pupil, painter::core::Colors::White, GlowShare),
                   .Core      = Pupil,
                   .PupilEdge = Color::Between(Pupil, painter::core::Colors::White, EdgeShare),
                   .Haze      = Color::Between(Deep, painter::core::Colors::Black, HazeShare)};
}

namespace Logo {

Color BodyColor(const Palette &Hues, Vec At)
{
    return Ramp(BodyGradient.At(At), {BoxDuppoBodyStops[0].At, Hues.Light}, {BoxDuppoBodyStops[1].At, Hues.Deep});
}

Color SocketColor(const Palette &Hues, Vec At)
{
    return Ramp(SocketGradient.At(At), {BoxDuppoSocketStops[0].At, Hues.Deep}, {BoxDuppoSocketStops[1].At, Hues.Lip});
}

Color WhiteColor(const Palette &Hues, Vec At)
{
    return Ramp(WhiteGradient.At(At), {BoxDuppoWhiteStops[0].At, Hues.Ring}, {BoxDuppoWhiteStops[1].At, Hues.RingEdge});
}

Color PupilColor(const Palette &Hues, Vec At)
{
    return Ramp(PupilGradient.At(At), {BoxDuppoPupilStops[0].At, Hues.Glow}, {BoxDuppoPupilStops[1].At, Hues.Core},
                {BoxDuppoPupilStops[2].At, Hues.PupilEdge});
}

Color MoonColor(const Palette &Hues, int Which, Vec At)
{
    return Ramp(MoonGradients[Which].At(At), {BoxDuppoMoonStops[0].At, Hues.Light}, {BoxDuppoMoonStops[1].At, Hues.Deep});
}

}  // namespace Logo

}  // namespace duppo
