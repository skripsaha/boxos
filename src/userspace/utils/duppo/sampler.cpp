/*
 * sampler.cpp — Duppo at one instant, the colour of any point. See sampler.h.
 */

#include "sampler.h"

#include <algorithm>
#include <cmath>

namespace duppo {

namespace {

/* The lids: where they meet when shut is a bowed line, and open they lie a
 * little beyond the socket; the crease is a line of the deep colour along
 * their edges. */
constexpr float LidMeet       = 0.18f;
constexpr float LidSag        = 0.30f;
constexpr float LidOpen       = 1.15f;
constexpr float CreaseHalf    = 0.75f;
constexpr float CreaseOpacity = 0.85f;

/* The eye is a picture on the front of the ball and goes round it. A point of
 * the ball is the picture's only if, taken back through the turn, it still
 * faces the front (a depth of at least EyeAway); at the ball's very edge, past
 * EyeLimb of the radius squared, it has no depth to take back; and an edge of
 * the eye is softened by how squarely it faces us, never by less than Squarest. */
constexpr float EyeAway  = 0.05f;
constexpr float EyeLimb  = 0.98f;
constexpr float Squarest = 0.3f;

/* A bubble that lies behind the ball is mixed with the haze, at most this much of 255. */
constexpr float FarHaze = 90.0f;

/* Over covers Share of a sub-pixel: over a see-through Under it is Over at that
 * opacity, which the display blends with what lies beneath, and over a colour
 * the mix of the two. */
Color Lay(Color Under, Color Over, float Share)
{
    if (Share <= 0.0f) return Under;
    if (Share >= 1.0f) return Over;
    return Color::Over(Under, Over, static_cast<std::uint8_t>(Share * 255.0f + 0.5f));
}

constexpr float BodyReach = 1.25f;   /* the body's bounds in radii, for its squash */

}  // namespace

Sampler::Sampler(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, const Palette &Hues, bool Whole,
                 Vec Sub)
    : Body(Body), Eye(Eye), Turning(Turning), Moons(Moons), Hues(Hues), Whole(Whole), Sub(Sub), Near(std::max(Sub.X, Sub.Y))
{
}

/* How much of a sample a shape covers, from how far outside it the sample's
 * middle lies, the sample's width counted along Toward, the way out. Whole:
 * on a text page, whose sixteen colours have no mixes, or when the person set
 * the edge crisp — whole or none. */
float Sampler::Cover(float Far, Vec Toward) const
{
    if (Far > Near) return 0.0f;
    if (Far < -Near) return 1.0f;
    const Vec   Way   = Normalised(Toward);
    const float Width = Sub.X * std::fabs(Way.X) + Sub.Y * std::fabs(Way.Y);
    const float Share = std::clamp(0.5f - Far / std::max(Width, 0.2f * std::min(Sub.X, Sub.Y)), 0.0f, 1.0f);
    return Whole ? (Share >= 0.5f ? 1.0f : 0.0f) : Share;
}

Color Sampler::At(Vec Point, bool &Reached) const
{
    Layer Layers[1 + Logo::MoonCount];
    int   Count = 0;

    const Vec   Away    = Point - Body.Center;
    const Vec   Local   = Body.Warp.Apply(Away) / Body.Scale;
    const float Radius  = Logo::Body.Radius;
    const float BodyFar = (Length(Local) - Radius) * Body.Scale;
    if (BodyFar <= Near) {
        const float Share = Cover(BodyFar, Away);
        const float Dome  = std::max(Radius * Radius - Dot(Local, Local), 0.0f);
        if (Share > 0.0f) Layers[Count++] = {std::sqrt(Dome) * Body.Scale, Share, -1, Local};
    }
    for (int At = 0; At < Moons.Count; At++) {
        const Moon &One    = Moons.Moons[At];
        const float Scale  = Body.Scale * One.Grow;
        const Vec   Offset = Point - One.Center;
        const Vec   Inside = Offset / Scale;
        const float Size   = Logo::Moons[At].Radius;
        const float Share  = Cover((Length(Inside) - Size) * Scale, Offset);
        if (Share <= 0.0f) continue;
        const float Dome = std::max(Size * Size - Dot(Inside, Inside), 0.0f);
        Layers[Count++]  = {One.Depth + std::sqrt(Dome) * Scale, Share, At, Inside};
    }
    Reached = Count > 0;

    for (int Next = 1; Next < Count; Next++)
        for (int Back = Next; Back > 0 && Layers[Back].Depth < Layers[Back - 1].Depth; Back--) std::swap(Layers[Back], Layers[Back - 1]);

    Color Ink = Color::Through();
    for (int At = 0; At < Count; At++) Ink = Layers[At].Which < 0 ? Sphere(Layers[At].Local, Layers[At].Share, Ink) : Little(Layers[At], Ink);
    return Ink;
}

/* The ball's colour with the eye on it, all whole, laid over Under by the share
 * of the sample the ball covers: the eye is on the ball, so it takes no more of
 * an edge than the ball does. */
Color Sampler::Sphere(Vec Local, float Share, Color Under) const
{
    const Vec Seen = Logo::Body.Center + Local;
    return Lay(Under, Eyes(Local, Seen, Logo::BodyColor(Hues, Seen)), Share);
}

/* The eye lies on the ball's front as the logo draws it, and the ball is turned
 * in space: a point of the ball in view is taken back through the turn to the
 * place it has on the picture, and the eye is drawn there. Its shading is the
 * picture's, so it goes round with the ball; the shape of its edges is
 * softened by how squarely it faces us. */
Color Sampler::Eyes(Vec Local, Vec Seen, Color Under) const
{
    const float Radius = Logo::Body.Radius;
    const float Flat   = Dot(Local, Local) / (Radius * Radius);
    if (Flat >= EyeLimb) return Under;

    const Vec3 Front = {Local.X / Radius, Local.Y / Radius, std::sqrt(1.0f - Flat)};
    const Vec3 Round = Turning.Pose.Undone(Front);
    if (Round.Z < EyeAway) return Under;

    const Vec   Tex        = Logo::Body.Center + Vec{Round.X, Round.Y} * Radius;
    const float Scale      = Body.Scale * std::clamp(std::fabs(Turning.Pose.M[2][2]), Squarest, 1.0f);
    const Vec   FromSocket = Tex - Logo::Socket.Center;
    const float SocketFar  = (Length(FromSocket) - Logo::Socket.Radius) * Scale;
    if (SocketFar > Near) return Under;

    Under = Lay(Under, Logo::SocketColor(Hues, Tex), Cover(SocketFar, FromSocket));
    const Vec FromWhite = Tex - Logo::White.Center;
    Under = Lay(Under, Logo::WhiteColor(Hues, Tex), Cover((Length(FromWhite) - Logo::White.Radius) * Scale, FromWhite));
    Under = Pupil(Tex, Scale, Under);
    return Eye.Shut > 0.01f ? Lids(Tex, Seen, SocketFar, Scale, Under) : Under;
}

/* The pupil is an ellipse that slides in the white as the eye looks; its
 * shading goes with it. */
Color Sampler::Pupil(Vec Tex, float Scale, Color Under) const
{
    const Vec   Moved  = Tex - Eye.Gaze * Logo::GazeReach;
    const Vec   From   = Moved - Logo::PupilCenter;
    const float Across = From.X / Logo::PupilReach.X, Down = From.Y / Logo::PupilReach.Y;
    const float Far    = (std::sqrt(Across * Across + Down * Down) - 1.0f) * (Logo::PupilReach.X + Logo::PupilReach.Y) * 0.5f * Scale;
    return Lay(Under, Logo::PupilColor(Hues, Moved), Cover(Far, From));
}

/* Two lids close on the socket, the upper from above and the lower, slower,
 * from below, to meet on a bowed line; they are the body's own colour, so the
 * eye goes under the sphere's skin, and a crease is drawn along their edges. */
Color Sampler::Lids(Vec Tex, Vec Seen, float SocketFar, float Scale, Color Under) const
{
    const float Radius = Logo::Socket.Radius;
    const Vec   From   = Tex - Logo::Socket.Center;
    const float Bow    = From.X / Radius;
    const float Meet   = LidMeet * Radius + LidSag * Radius * (1.0f - Bow * Bow);
    const float Upper  = -LidOpen * Radius + (Meet + LidOpen * Radius) * Eye.Shut;
    const float Lower  = LidOpen * Radius + (Meet - LidOpen * Radius) * Eye.Shut * Eye.Shut * Eye.Shut;

    const float Closed = std::max(SocketFar, std::min(From.Y - Upper, Lower - From.Y) * Scale);
    Under = Lay(Under, Logo::BodyColor(Hues, Seen), Cover(Closed, {0.0f, 1.0f}));

    const float Edge   = (std::min(std::fabs(From.Y - Upper), std::fabs(From.Y - Lower)) - CreaseHalf) * Scale;
    const float Inside = (Length(From) - Radius + 0.3f) * Scale;
    return Lay(Under, Hues.Deep, CreaseOpacity * Cover(std::max(Edge, Inside), {0.0f, 1.0f}));
}

/* A little bubble, in the colours of the logo's own for it, dimmed toward the
 * haze where it lies behind the ball. */
Color Sampler::Little(const Layer &Hit, Color Under) const
{
    Color Hue = Logo::MoonColor(Hues, Hit.Which, Logo::Moons[Hit.Which].Center + Hit.Local);
    const float Behind = -Moons.Moons[Hit.Which].Depth / (Logo::Body.Radius * Body.Scale);
    if (Behind > 0.0f) Hue = Color::Over(Hue, Hues.Haze, static_cast<std::uint8_t>(std::min(Behind, 1.0f) * FarHaze));
    return Lay(Under, Hue, Hit.Share);
}

Reach ReachOf(const Flight &Body, const Orbit &Moons)
{
    const float Radius = Logo::Body.Radius * Body.Scale * BodyReach;
    Reach       Made{Body.Center.X - Radius, Body.Center.Y - Radius, Body.Center.X + Radius, Body.Center.Y + Radius};
    for (int At = 0; At < Moons.Count; At++) {
        const Moon &One  = Moons.Moons[At];
        const float Size = Logo::Moons[At].Radius * Body.Scale * One.Grow;
        Made.Left   = std::min(Made.Left, One.Center.X - Size);
        Made.Top    = std::min(Made.Top, One.Center.Y - Size);
        Made.Right  = std::max(Made.Right, One.Center.X + Size);
        Made.Bottom = std::max(Made.Bottom, One.Center.Y + Size);
    }
    return Made;
}

}  // namespace duppo
