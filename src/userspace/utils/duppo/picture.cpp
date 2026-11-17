#include "picture.h"

#include <algorithm>
#include <cmath>

#include "painter/tui/fine_canvas.h"
#include "painter/tui/mosaic.h"

namespace duppo {

namespace {

using painter::core::Rect;
using painter::tui::Mosaic;

/* A sub-pixel is half a unit across and one down. */
constexpr float SubAcross = 0.5f;
constexpr float SubDown   = 1.0f;

/* The lids: where they meet when shut is a bowed line, and open they lie a
 * little beyond the socket; the crease is a line of the deep colour along
 * their edges. */
constexpr float LidMeet       = 0.18f;
constexpr float LidSag        = 0.30f;
constexpr float LidOpen       = 1.15f;
constexpr float CreaseHalf    = 0.75f;
constexpr float CreaseOpacity = 0.85f;

constexpr float BodyReach = 1.25f;   /* the body's bounds in radii, for its squash */

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

Rect Joined(Rect A, Rect B)
{
    if (A.Empty()) return B;
    if (B.Empty()) return A;
    std::int32_t Left = std::min(A.X, B.X), Top = std::min(A.Y, B.Y);
    std::int32_t Right = std::max(A.X + A.Width, B.X + B.Width), Bottom = std::max(A.Y + A.Height, B.Y + B.Height);
    return {Left, Top, Right - Left, Bottom - Top};
}

/* How much of a sub-pixel a shape covers, from how far outside it the
 * sub-pixel's middle lies, the sub-pixel's width counted along Toward, the
 * way out. On a text page, whose sixteen colours have no mixes, or when the
 * person set the edge crisp, whole or none. */
float Cover(float Far, Vec Toward, bool Whole)
{
    if (Far > SubDown) return 0.0f;
    if (Far < -SubDown) return 1.0f;
    const Vec   Way   = Normalised(Toward);
    const float Width = SubAcross * std::fabs(Way.X) + SubDown * std::fabs(Way.Y);
    const float Share = std::clamp(0.5f - Far / std::max(Width, 0.1f), 0.0f, 1.0f);
    return Whole ? (Share >= 0.5f ? 1.0f : 0.0f) : Share;
}

/* Over covers Share of a sub-pixel: over a see-through Under it is Over at that
 * opacity, which the display blends with what lies beneath, and over a colour
 * the mix of the two. */
Color Lay(Color Under, Color Over, float Share)
{
    if (Share <= 0.0f) return Under;
    if (Share >= 1.0f) return Over;
    return Color::Over(Under, Over, static_cast<std::uint8_t>(Share * 255.0f + 0.5f));
}

/* What Duppo is at one instant, and the colour of any point of the glass. */
class Sampler {
public:
    Sampler(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, const Palette &Hues, bool Whole)
        : Body(Body), Eye(Eye), Turning(Turning), Moons(Moons), Hues(Hues), Whole(Whole)
    {
    }

    /* The colour at Point, translucent where Duppo covers part of it and
     * see-through where it covers none, and whether Duppo is there at all. */
    Color At(Vec Point, bool &Reached) const;

private:
    /* A disc that covers the sample, and how near the person it lies there:
     * Which is -1 for the ball and the bubble's number for a bubble, Local the
     * sample in that disc's own logo units. */
    struct Layer {
        float Depth;
        float Share;
        int   Which;
        Vec   Local;
    };

    Color Sphere(Vec Local, float Share, Color Under) const;
    Color Eyes(Vec Local, Vec Seen, Color Under) const;
    Color Pupil(Vec Tex, float Scale, Color Under) const;
    Color Lids(Vec Tex, Vec Seen, float SocketFar, float Scale, Color Under) const;
    Color Little(const Layer &Hit, Color Under) const;

    const Flight  &Body;
    const Face    &Eye;
    const Spin    &Turning;
    const Orbit   &Moons;
    const Palette &Hues;
    bool           Whole;
};

Color Sampler::At(Vec Point, bool &Reached) const
{
    Layer Layers[1 + Logo::MoonCount];
    int   Count = 0;

    const Vec   Away    = Point - Body.Center;
    const Vec   Local   = Body.Warp.Apply(Away) / Body.Scale;
    const float Radius  = Logo::Body.Radius;
    const float BodyFar = (Length(Local) - Radius) * Body.Scale;
    if (BodyFar <= SubDown) {
        const float Share = Cover(BodyFar, Away, Whole);
        const float Dome  = std::max(Radius * Radius - Dot(Local, Local), 0.0f);
        if (Share > 0.0f) Layers[Count++] = {std::sqrt(Dome) * Body.Scale, Share, -1, Local};
    }
    for (int At = 0; At < Moons.Count; At++) {
        const Moon &One    = Moons.Moons[At];
        const float Scale  = Body.Scale * One.Grow;
        const Vec   Offset = Point - One.Center;
        const Vec   Inside = Offset / Scale;
        const float Size   = Logo::Moons[At].Radius;
        const float Share  = Cover((Length(Inside) - Size) * Scale, Offset, Whole);
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
    if (SocketFar > SubDown) return Under;

    Under = Lay(Under, Logo::SocketColor(Hues, Tex), Cover(SocketFar, FromSocket, Whole));
    const Vec FromWhite = Tex - Logo::White.Center;
    Under = Lay(Under, Logo::WhiteColor(Hues, Tex), Cover((Length(FromWhite) - Logo::White.Radius) * Scale, FromWhite, Whole));
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
    return Lay(Under, Logo::PupilColor(Hues, Moved), Cover(Far, From, Whole));
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
    Under = Lay(Under, Logo::BodyColor(Hues, Seen), Cover(Closed, {0.0f, 1.0f}, Whole));

    const float Edge   = (std::min(std::fabs(From.Y - Upper), std::fabs(From.Y - Lower)) - CreaseHalf) * Scale;
    const float Inside = (Length(From) - Radius + 0.3f) * Scale;
    return Lay(Under, Hues.Deep, CreaseOpacity * Cover(std::max(Edge, Inside), {0.0f, 1.0f}, Whole));
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

/* The cells Duppo can touch: the body's bounds, squash included, and every
 * bubble's, the cell round them that a soft edge reaches into taken in. */
Rect Reach(const Flight &Body, const Orbit &Moons, Rect Glass)
{
    const float Radius = Logo::Body.Radius * Body.Scale * BodyReach;
    float Left = Body.Center.X - Radius, Top = Body.Center.Y - Radius;
    float Right = Body.Center.X + Radius, Bottom = Body.Center.Y + Radius;
    for (int At = 0; At < Moons.Count; At++) {
        const Moon &One  = Moons.Moons[At];
        const float Size = Logo::Moons[At].Radius * Body.Scale * One.Grow;
        Left   = std::min(Left, One.Center.X - Size);
        Top    = std::min(Top, One.Center.Y - Size);
        Right  = std::max(Right, One.Center.X + Size);
        Bottom = std::max(Bottom, One.Center.Y + Size);
    }
    return painter::tui::FineCanvas::Reaching(Left, Top, Right, Bottom).Intersect(Glass);
}

}  // namespace

Rect Picture::Draw(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, painter::tui::Frame &Overlay)
{
    Overlay.Fill(Shown, painter::tui::Cell::TransparentCell());

    const bool    Whole = !Softened || Overlay.Glass() == painter::tui::GlassKind::Text;
    const Sampler Seen(Body, Eye, Turning, Moons, Colours, Whole);
    const Rect    Reached = Reach(Body, Moons, {0, 1, Cells.Width, Cells.Height - 1});

    for (std::int32_t Row = Reached.Y; Row < Reached.Y + Reached.Height; Row++) {
        for (std::int32_t Col = Reached.X; Col < Reached.X + Reached.Width; Col++) {
            Mosaic Wanted;
            bool Touched = false;
            for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
                const Vec Middle{static_cast<float>(Col) + (Place % 2 == 0 ? 0.25f : 0.75f),
                                 static_cast<float>(Row * 2) + (Place < 2 ? 0.5f : 1.5f)};
                bool Here = false;
                Wanted.At[Place] = Seen.At(Middle, Here);
                Touched       = Touched || Here;
            }
            if (Touched) Overlay.Put({Col, Row}, painter::tui::ResolveMosaic(Wanted, Overlay.Glass(), Overlay.Mosaics()));
        }
    }

    const Rect Changed = Joined(Shown, Reached);
    Shown              = Reached;
    return Changed.Intersect(Cells);
}

}  // namespace duppo
