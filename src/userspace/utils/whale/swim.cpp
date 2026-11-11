#include "swim.h"

#include <algorithm>
#include <cmath>

#include "box/cpu.h"

namespace whale {

namespace {

constexpr float Pi         = 3.14159265f;
constexpr float BeatRate   = 2.0f * Pi * 0.55f;   /* strokes of the tail: a little over one in two seconds */
constexpr std::size_t MostDrops = 96;

std::uint64_t Seed = 1;

float Unit()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return static_cast<float>(Seed >> 40) / static_cast<float>(1ull << 24);
}

float Between(float Low, float High)
{
    return Low + (High - Low) * Unit();
}

float Wrap(float Angle)
{
    while (Angle > Pi) Angle -= 2.0f * Pi;
    while (Angle < -Pi) Angle += 2.0f * Pi;
    return Angle;
}

float Distance(Vec A, Vec B)
{
    return std::hypot(A.X - B.X, A.Y - B.Y);
}

Vec Unity(Vec Direction)
{
    float Size = std::hypot(Direction.X, Direction.Y);
    return Size > 1e-4f ? Direction * (1.0f / Size) : Vec{1.0f, 0.0f};
}

}  // namespace

float Swimmer::Girth(float Along)
{
    constexpr float Widest = 0.33f;
    if (Along < Widest) return std::pow(std::sin(Pi / 2.0f * Along / Widest), 0.42f);
    float Back = (Along - Widest) / (1.0f - Widest);
    return 0.12f + 0.88f * std::pow(1.0f - Back, 1.1f);
}

void Swimmer::Begin(int PixelsAcross, int PixelsDown)
{
    Seed   = cpu_rdtsc() | 1;
    Across = PixelsAcross;
    Down   = PixelsDown;

    Length = std::min(std::clamp(Across / 3.2f, 28.0f, 80.0f), Down * 0.6f);
    Cruise = Length * 0.3f;
    MinX   = Length * 0.2f;
    MaxX   = Across - MinX;
    MinY   = 4.0f + Length * 0.2f;
    MaxY   = Down - Length * 0.2f - 2.0f;

    Heading = Unit() < 0.5f ? 0.0f : Pi;
    Course  = Heading;
    Roll    = std::cos(Heading) > 0.0f ? 1.0f : -1.0f;

    /* Begun with the whole body on the glass: the tail trails behind a head
     * that starts far enough in from its own side. */
    float Room = Length * 1.3f;
    Head.X = Roll > 0.0f ? Between(MinX + Room, std::max(MinX + Room, MaxX)) : Between(MinX, std::max(MinX, MaxX - Room));
    Head.Y = Between(MinY, MaxY);

    Vec Ahead{std::cos(Heading), std::sin(Heading)};
    int Marks = static_cast<int>(Room) + 2;
    for (int Back = Marks - 1; Back >= 0; Back--) Trail.push_back({Head - Ahead * static_cast<float>(Back), Roll});
    TrailLength = static_cast<float>(Marks - 1);

    ToSpout = Between(3.0f, 6.0f);
    Shape();
}

void Swimmer::Step(float Seconds)
{
    Time += Seconds;
    Beat += Seconds * BeatRate;
    FinSwing = std::sin(Time * 1.7f);
    Steer(Seconds);
    Advance(Seconds);
    Shape();
    Release(Seconds);
}

/* A course held for some seconds and then changed a little, now and then
 * reversed; near an edge the course gives way to a turn toward the middle,
 * never sharper than the body can bend. */
void Swimmer::Steer(float Seconds)
{
    ToCourse -= Seconds;
    if (ToCourse <= 0.0f) {
        ToCourse = Between(4.0f, 9.0f);
        float Reversed = Unit() < 0.12f ? Pi : 0.0f;
        Course = (std::cos(Heading) >= 0.0f ? 0.0f : Pi) + Reversed + Between(-0.5f, 0.5f);
    }

    float Sideways = Length * 1.3f, Upright = Length * 0.7f;
    float Near = std::max({1.0f - (Head.X - MinX) / Sideways, 1.0f - (MaxX - Head.X) / Sideways,
                           1.0f - (Head.Y - MinY) / Upright, 1.0f - (MaxY - Head.Y) / Upright, 0.0f});
    Near = std::min(1.0f, Near * 2.0f);

    Vec Wanted{std::cos(Course), std::sin(Course)};
    if (Near > 0.0f) {
        Vec Middle{(MinX + MaxX) / 2.0f, (MinY + MaxY) / 2.0f};
        Wanted = Wanted * (1.0f - Near) + Unity(Middle - Head) * Near;
    }

    float MostTurn = Cruise / (Length * 0.5f);
    float Turn     = std::clamp(Wrap(std::atan2(Wanted.Y, Wanted.X) - Heading) * 1.8f, -MostTurn, MostTurn);
    Heading = Wrap(Heading + Turn * Seconds);

    float Side = std::clamp(std::cos(Heading) * 2.2f, -1.0f, 1.0f);
    Roll += (Side - Roll) * std::min(1.0f, Seconds * 2.5f);
}

void Swimmer::Advance(float Seconds)
{
    float Speed = Cruise * (1.0f + 0.1f * std::sin(2.0f * Beat));
    Vec   Last  = Head;
    Head = Head + Vec{std::cos(Heading), std::sin(Heading)} * (Speed * Seconds);
    Head.X = std::clamp(Head.X, MinX, MaxX);
    Head.Y = std::clamp(Head.Y, MinY, MaxY);

    Trail.push_back({Head, Roll});
    TrailLength += Distance(Last, Head);
    while (Trail.size() > 2 && TrailLength - Distance(Trail[0].At, Trail[1].At) >= Length) {
        TrailLength -= Distance(Trail[0].At, Trail[1].At);
        Trail.erase(Trail.begin());
    }
}

/* The place on the head's path a distance back from the head. */
Swimmer::Mark Swimmer::Behind(float Back) const
{
    for (std::size_t At = Trail.size() - 1; At > 0; At--) {
        float Piece = Distance(Trail[At].At, Trail[At - 1].At);
        if (Back <= Piece) {
            float Share = Piece > 0.0f ? Back / Piece : 0.0f;
            Vec   Place = Trail[At].At + (Trail[At - 1].At - Trail[At].At) * Share;
            return {Place, Trail[At].Facing + (Trail[At - 1].Facing - Trail[At].Facing) * Share};
        }
        Back -= Piece;
    }
    return Trail.front();
}

/* The spine is the path walked back from the head, with the wave of the
 * stroke laid across it: still at the snout, widest at the tail. */
void Swimmer::Shape()
{
    Vec Path[SpineSegments + 1];
    for (int At = 0; At <= SpineSegments; At++) {
        Mark Place = Behind(Length * At / SpineSegments);
        Path[At]   = Place.At;
        Facing[At] = Place.Facing;
    }
    for (int At = 0; At <= SpineSegments; At++) {
        Vec   Ahead  = Unity(Path[std::max(At - 1, 0)] - Path[std::min(At + 1, SpineSegments)]);
        float Along  = static_cast<float>(At) / SpineSegments;
        float Sway   = Length * 0.06f * (0.1f + 0.9f * Along * Along);
        float Offset = Sway * std::sin(Beat - 2.0f * Pi * 0.9f * Along);
        Spine[At]    = Path[At] + Vec{-Ahead.Y, Ahead.X} * Offset;
    }
}

/* The spout, now and then and only when the back is up and the whale goes
 * level, and the bubbles the tail leaves. Drops fall and fade by themselves. */
void Swimmer::Release(float Seconds)
{
    const int Hole = static_cast<int>(BlowholeAt * SpineSegments);

    ToSpout -= Seconds;
    if (ToSpout <= 0.0f && Spouting <= 0.0f && std::fabs(Facing[Hole]) > 0.8f && std::fabs(std::sin(Heading)) < 0.5f) {
        Spouting = 0.9f;
        ToSpout  = Between(9.0f, 15.0f);
    }
    if (Spouting > 0.0f) {
        Spouting -= Seconds;
        Vec   Ahead = Unity(Spine[Hole - 1] - Spine[Hole + 1]);
        Vec   Back  = Vec{-Ahead.Y, Ahead.X} * (Facing[Hole] > 0.0f ? -1.0f : 1.0f);
        Vec   Mouth = Spine[Hole] + Back * (Length * WidestHalf * Girth(BlowholeAt) * 0.9f);
        int   Count = static_cast<int>(Seconds * 45.0f + Unit());
        for (int Each = 0; Each < Count && Drops.size() < MostDrops; Each++) {
            Drop Spray;
            Spray.At    = Mouth;
            Spray.Move  = {Ahead.X * Cruise * 0.6f + Between(-0.16f, 0.16f) * Length, -Between(0.55f, 0.85f) * Length};
            Spray.Life  = Between(0.9f, 1.2f);
            Spray.Spray = true;
            Drops.push_back(Spray);
        }
    }

    ToBubble -= Seconds;
    if (ToBubble <= 0.0f && Drops.size() < MostDrops) {
        ToBubble = Between(0.3f, 0.8f);
        Drop Bubble;
        Bubble.At    = Spine[SpineSegments] + Vec{Between(-1.0f, 1.0f), Between(-1.5f, 1.5f)};
        Bubble.Move  = {Between(-0.02f, 0.02f) * Length, -Between(0.05f, 0.1f) * Length};
        Bubble.Life  = Between(2.0f, 3.2f);
        Bubble.Phase = Between(0.0f, 2.0f * Pi);
        Drops.push_back(Bubble);
    }

    for (Drop &One : Drops) {
        One.Age += Seconds;
        if (One.Spray) One.Move.Y += Length * 1.6f * Seconds;
        else One.At.X += std::sin(One.Age * 3.0f + One.Phase) * Length * 0.04f * Seconds;
        One.At = One.At + One.Move * Seconds;
    }
    Drops.erase(std::remove_if(Drops.begin(), Drops.end(),
                               [this](const Drop &One) {
                                   return One.Age > One.Life || One.At.Y < 2.0f || One.At.Y >= Down || One.At.X < 0.0f ||
                                          One.At.X >= Across;
                               }),
                Drops.end());
}

}  // namespace whale
