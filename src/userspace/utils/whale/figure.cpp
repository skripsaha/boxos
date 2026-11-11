#include "figure.h"

#include <algorithm>
#include <cmath>

namespace whale {

namespace {

using painter::core::Color;

/* A blue whale: a deep blue back that turns to slate along the flank and to
 * pale at the belly, which has its throat grooves. Every one keeps its
 * family on a sixteen-colour console too: blue, light blue, white. */
constexpr Color BackTop = Color::Rgb(0x14, 0x32, 0x7A);
constexpr Color Back    = Color::Rgb(0x1E, 0x4A, 0x98);
constexpr Color Mottle  = Color::Rgb(0x2E, 0x62, 0xB0);
constexpr Color Flank   = Color::Rgb(0x3A, 0x70, 0xBA);
constexpr Color Lower   = Color::Rgb(0x78, 0xA8, 0xDC);
constexpr Color Belly   = Color::Rgb(0xC4, 0xDE, 0xF2);
constexpr Color Groove  = Color::Rgb(0x92, 0xB8, 0xDC);
constexpr Color Fluke   = Color::Rgb(0x1A, 0x3C, 0x86);
constexpr Color Fin     = Color::Rgb(0x56, 0x8C, 0xCE);
constexpr Color Lip     = Color::Rgb(0x10, 0x26, 0x58);
constexpr Color Pupil   = Color::Rgb(0x04, 0x08, 0x16);
constexpr Color Glint   = Color::Rgb(0xF0, 0xF6, 0xFF);

std::uint32_t Hash(int A, int B)
{
    std::uint32_t Mixed = static_cast<std::uint32_t>(A) * 374761393u + static_cast<std::uint32_t>(B) * 668265263u;
    Mixed = (Mixed ^ (Mixed >> 13)) * 1274126177u;
    return Mixed ^ (Mixed >> 16);
}

float Dot(Vec A, Vec B)
{
    return A.X * B.X + A.Y * B.Y;
}

Vec Unity(Vec Direction)
{
    float Size = std::hypot(Direction.X, Direction.Y);
    return Size > 1e-4f ? Direction * (1.0f / Size) : Vec{1.0f, 0.0f};
}

}  // namespace

Figure::Figure()
{
    for (std::size_t At = 0; At < Girths.size(); At++) Girths[At] = Swimmer::Girth(static_cast<float>(At) / (Girths.size() - 1));
}

float Figure::Radius(float Along) const
{
    float Place = std::clamp(Along, 0.0f, 1.0f) * (Girths.size() - 1);
    std::size_t Low = std::min(static_cast<std::size_t>(Place), Girths.size() - 2);
    return Length * WidestHalf * (Girths[Low] + (Girths[Low + 1] - Girths[Low]) * (Place - Low));
}

void Figure::AddLeaf(Vec From, Vec To, float Width, Color Ink)
{
    Tails[TailCount++] = {From, To, Width, Ink};
}

void Figure::Pose(const Swimmer &Whale)
{
    Length = Whale.Length;
    Least  = Most = Whale.Spine[0];
    for (int At = 0; At < SpineSegments; At++) {
        Vec Step = Whale.Spine[At + 1] - Whale.Spine[At];
        Segments[At] = {Whale.Spine[At], Unity(Step), {}, std::hypot(Step.X, Step.Y), static_cast<float>(At) / SpineSegments,
                        (Whale.Facing[At] + Whale.Facing[At + 1]) / 2.0f};
        Segments[At].Side = {Segments[At].Toward.Y, -Segments[At].Toward.X};
    }
    float Reach = Length * 0.3f;
    for (const Vec &Point : Whale.Spine) {
        Least = {std::min(Least.X, Point.X - Reach), std::min(Least.Y, Point.Y - Reach)};
        Most  = {std::max(Most.X, Point.X + Reach), std::max(Most.Y, Point.Y + Reach)};
    }

    const Segment &Arm = Segments[7];
    float          Swing = 0.55f + 0.4f * Whale.FinSwing;
    Vec Root = Arm.From + Arm.Side * (Arm.Facing * Radius(Arm.Along) * 0.5f);
    Vec Dir  = Arm.Toward * std::cos(Swing) + Arm.Side * (Arm.Facing * std::sin(Swing));
    Pectoral = {Root, Root + Dir * (Length * 0.2f), Length * 0.05f * (0.4f + 0.6f * std::fabs(Arm.Facing)), Fin};

    TailCount = 0;
    const Segment &Ridge = Segments[17];
    Vec Up   = Ridge.Side * (Ridge.Facing >= 0.0f ? -1.0f : 1.0f);
    Vec Base = Ridge.From + Up * (Radius(Ridge.Along) * 0.7f);
    AddLeaf(Base, Base + (Up * 0.6f + Ridge.Toward * 0.8f) * (Length * 0.075f), Length * 0.022f, Back);

    const Segment &End   = Segments[SpineSegments - 1];
    Vec            Tip   = End.From + End.Toward * End.Length;
    Vec            Joint = Tip - End.Toward * (Length * 0.02f);
    constexpr float Spread = 1.3f;
    for (float Way : {1.0f, -1.0f}) {
        Vec Out = End.Toward * std::cos(Spread) + End.Side * (Way * std::sin(Spread));
        AddLeaf(Joint, Joint + Out * (Length * 0.24f), Length * 0.045f, Fluke);
    }

    const Segment &Head = Segments[4];
    EyeShown  = std::fabs(Head.Facing) > 0.35f;
    EyeRadius = std::max(0.9f, Length * 0.02f);
    Eye       = Head.From;
}

bool Figure::Reaches(Vec Centre, float Radius) const
{
    const float Far = Length * WidestHalf + Radius;
    for (const Segment &One : Segments) {
        Vec   From  = Centre - One.From;
        float Reach = One.Length + Far;
        if (Dot(From, From) <= Reach * Reach) return true;
    }
    auto Near = [&](const Leaf &Shape) {
        Vec   Middle = (Shape.From + Shape.To) * 0.5f, Gap = Centre - Middle;
        float Reach  = std::hypot(Shape.To.X - Shape.From.X, Shape.To.Y - Shape.From.Y) * 0.5f + Shape.Width + Radius;
        return Dot(Gap, Gap) <= Reach * Reach;
    };
    if (Near(Pectoral)) return true;
    for (int At = 0; At < TailCount; At++)
        if (Near(Tails[At])) return true;
    return false;
}

bool Figure::Within(const Leaf &Shape, Vec Place) const
{
    Vec   Along = Shape.To - Shape.From, Off = Place - Shape.From;
    float Size2 = Dot(Along, Along);
    float U     = Size2 > 0.0f ? std::clamp(Dot(Off, Along) / Size2, 0.0f, 1.0f) : 0.0f;
    Vec   Gap   = Off - Along * U;
    float Reach = Shape.Width * 2.0f * std::sqrt(U * (1.0f - U));
    return Dot(Gap, Gap) <= Reach * Reach;
}

Color Figure::Skin(float Along, float Facing, float Offset, float Radius) const
{
    float Across = std::clamp(Facing * Offset / Radius, -1.0f, 1.0f);

    if (std::fabs(Facing) > 0.35f && Along < 0.2f && std::fabs(Across - (0.58f - 1.5f * Along)) * Radius < 0.55f) return Lip;
    if (Across > 0.62f && Along > 0.07f && Along < 0.55f && static_cast<int>((Across - 0.62f) * 13.0f) % 2 == 1) return Groove;
    if (Across < -0.2f && Hash(static_cast<int>(Along * Length), static_cast<int>(std::floor(Offset))) % 29 == 0) return Mottle;
    if (Across < -0.68f) return BackTop;
    if (Across < -0.2f) return Back;
    if (Across < 0.22f) return Flank;
    if (Across < 0.58f) return Lower;
    return Belly;
}

Pixel Figure::BodyAt(Vec Place) const
{
    const Segment *Near = nullptr;
    float          Closest = 1e9f, Reached = 0.0f;
    Vec            Off;
    const float    Far = Length * WidestHalf;
    for (const Segment &One : Segments) {
        Vec   From = Place - One.From;
        float Reach = One.Length + Far;
        if (Dot(From, From) > Reach * Reach) continue;
        float Step = std::clamp(Dot(From, One.Toward), 0.0f, One.Length);
        Vec   Gap  = From - One.Toward * Step;
        float Dist = Dot(Gap, Gap);
        if (Dist < Closest) {
            Closest = Dist;
            Near    = &One;
            Reached = Step;
            Off     = Gap;
        }
    }
    if (!Near) return {};

    float Along  = Near->Along + (Near->Length > 0.0f ? Reached / Near->Length : 0.0f) / SpineSegments;
    float Radius = this->Radius(Along);
    if (Closest > Radius * Radius) return {};

    float Offset = Dot(Off, Near->Side);
    if (EyeShown) {
        Vec Gap = Place - Eye;
        float Dist = std::hypot(Gap.X, Gap.Y);
        if (EyeRadius >= 1.3f && std::hypot(Gap.X + EyeRadius * 0.4f, Gap.Y + EyeRadius * 0.4f) < EyeRadius * 0.42f) return {Glint, Part::Body};
        if (Dist <= EyeRadius) return {Pupil, Part::Body};
    }
    return {Skin(Along, Near->Facing, Offset, Radius), Part::Body};
}

Pixel Figure::At(Vec Place) const
{
    if (Within(Pectoral, Place)) return {Pectoral.Ink, Part::Fin};
    Pixel Body = BodyAt(Place);
    if (Body.Kind != Part::None) return Body;
    for (int At = 0; At < TailCount; At++)
        if (Within(Tails[At], Place)) return {Tails[At].Ink, Part::Tail};
    return {};
}

}  // namespace whale
