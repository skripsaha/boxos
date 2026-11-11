/* fauna_wing.cpp — the lives of what flies: the birds that fly, perch, sing
 * and peck, the owl that keeps the night, and the bats, butterflies and
 * fireflies that drift in their own hours. A bird's Y is always its feet: on
 * a branch, on the ground, or in the air under its belly. */

#include <algorithm>
#include <cmath>

#include "fauna.h"
#include "sway.h"

namespace forest {

namespace {

constexpr float Outside = 8.0f;

float Wide(const Habitat &Where) { return static_cast<float>(Where.Place.Size.Width); }

// Somewhere to land: a perch on a tree of the middle or the front that is on
// the glass, the highest of a few tried for what asks for height; false when
// there is none.
bool ChoosePerch(Animal &Bird, const Habitat &Where, bool High)
{
    const std::vector<Tree> &Trees = Where.Place.Trees;
    float BestUp = -1e9f;
    int   Found  = 0;
    Bird.Tree    = -1;
    Bird.Perch   = -1;
    for (int Try = 0; Try < 40 && Found < (High ? 6 : 1); Try++) {
        int         At    = Where.Chance.Count(0, static_cast<int>(Trees.size()) - 1);
        const Tree &Plant = Trees[static_cast<std::size_t>(At)];
        if (Plant.Layer == 0 || Plant.Perches.empty()) continue;
        int   Seat = Where.Chance.Count(0, static_cast<int>(Plant.Perches.size()) - 1);
        const Spot &Place = Plant.Perches[static_cast<std::size_t>(Seat)];
        float       Up    = Place.Y;
        if (Plant.Foot - Up < 10.0f || Plant.X + Place.X < 4.0f || Plant.X + Place.X > Wide(Where) - 4.0f) continue;
        Found++;
        if (Up <= BestUp) continue;
        BestUp     = Up;
        Bird.Tree  = At;
        Bird.Perch = Seat;
    }
    return Bird.Perch >= 0;
}

// Where the bird flies to: its perch, or a place on the ground.
void SetGoalToPerch(Animal &Bird, const Habitat &Where)
{
    float X, Y;
    if (PerchPlace(Where, Bird, X, Y)) {
        Bird.GoalX = X;
        Bird.GoalY = Y;
    }
}

// Straight toward the goal, slowing as it comes in to land. True on arrival.
bool Flies(Animal &Bird, float Speed, float Seconds)
{
    float Dx   = Bird.GoalX - Bird.X;
    float Dy   = Bird.GoalY - Bird.Y;
    float Away = std::sqrt(Dx * Dx + Dy * Dy);
    if (Away < 1.2f) return true;
    float Pace = std::min(Speed, 8.0f + Away * 1.3f);
    float Step = std::min(Pace * Seconds, Away);
    Bird.X += Dx / Away * Step;
    Bird.Y += Dy / Away * Step;
    Bird.Rise = std::sin(Bird.Time * 9.0f + Bird.Phase * 6.0f) * 0.9f;
    if (Dx < -0.3f) Bird.FacesLeft = true;
    if (Dx > 0.3f) Bird.FacesLeft = false;
    return false;
}

// Off to the ground, to somewhere in the meadow.
void ToGround(Animal &Bird, const Habitat &Where)
{
    Bird.Tree  = -1;
    Bird.Perch = -1;
    Bird.GoalX = Where.Chance.Between(8.0f, Wide(Where) - 8.0f);
    Bird.GoalY = LaneAt(Where.Place.Size, Where.Chance.Unit());
    Begin(Bird, Act::Fly, 0);
}

// Off to a perch, or to the ground if there is none.
void Seeks(Animal &Bird, const Habitat &Where, bool High)
{
    if (!ChoosePerch(Bird, Where, High)) {
        ToGround(Bird, Where);
        return;
    }
    SetGoalToPerch(Bird, Where);
    Begin(Bird, Act::Fly, 0);
}

// In from the edge of the glass, toward a perch.
void Enters(Animal &Bird, const Habitat &Where, bool High)
{
    bool FromLeft  = Where.Chance.Chance(0.5f);
    Bird.X         = FromLeft ? -Outside : Wide(Where) + Outside;
    Bird.Y         = Where.Chance.Between(10.0f, static_cast<float>(Where.Place.Size.Ground) * 0.7f);
    Bird.Leaving   = false;
    Bird.FacesLeft = !FromLeft;
    Seeks(Bird, Where, High);
}

// Out over the edge.
void Departs(Animal &Bird, const Habitat &Where)
{
    Bird.Leaving = true;
    Bird.GoalX   = Bird.X < Wide(Where) / 2.0f ? -Outside - 3.0f : Wide(Where) + Outside + 3.0f;
    Bird.GoalY   = Bird.Y - 10.0f;
    Bird.Tree    = -1;
    Bird.Perch   = -1;
    Bird.Doing   = Act::Fly;
}

// True while it is on the way in or out, or away: the kind's own life waits.
bool Comings(Animal &Bird, const Habitat &Where, float Seconds, bool High, float Speed)
{
    if (Bird.Doing == Act::Away) {
        Bird.Left -= Seconds;
        if (Bird.Left <= 0 && !Bird.Retire && Wants(Bird, Where)) Enters(Bird, Where, High);
        return true;
    }
    if (!Bird.Leaving && (Bird.Retire || !Wants(Bird, Where) || Startled(Bird, Where))) Departs(Bird, Where);
    if (!Bird.Leaving) return false;
    if (Flies(Bird, Speed * 1.3f, Seconds)) {
        Bird.Leaving = false;
        Begin(Bird, Act::Away, Where.Chance.Between(5.0f, 16.0f));
    }
    return true;
}

// Sitting where it sits: carried by the sway of the bough.
void Sits(Animal &Bird, const Habitat &Where)
{
    float X, Y;
    if (!PerchPlace(Where, Bird, X, Y)) return;
    Bird.X = X;
    Bird.Y = Y;
}

}  // namespace

bool PerchPlace(const Habitat &Where, const Animal &Beast, float &X, float &Y)
{
    if (Beast.Tree < 0 || Beast.Perch < 0) return false;
    const Tree &Plant = Where.Place.Trees[static_cast<std::size_t>(Beast.Tree)];
    const Spot &Seat  = Plant.Perches[static_cast<std::size_t>(Beast.Perch)];
    X = Plant.X + Seat.X + Sway(Where.Weather, Plant, Seat.Y);
    Y = Plant.Foot - Seat.Y;
    return true;
}

// ---- the birds ----------------------------------------------------------------

namespace {

void BirdNext(Animal &Bird, const Habitat &Where)
{
    Dice &Chance = Where.Chance;
    float Roll   = Chance.Unit();
    bool  Sings  = Where.Sky.Day > 0.3f && Where.Weather.Aloft.Rain < 0.3f;
    if (Roll < 0.4f && Sings) {
        Begin(Bird, Act::Sing, Chance.Between(3.0f, 7.0f));
    } else if (Roll < 0.7f) {
        ToGround(Bird, Where);
    } else {
        Seeks(Bird, Where, false);
    }
}

}  // namespace

void StepBird(Animal &Beast, const Habitat &Where, float Seconds)
{
    Beast.Left -= Seconds;
    if (Comings(Beast, Where, Seconds, false, 40.0f)) return;
    switch (Beast.Doing) {
    case Act::Fly:
        if (Beast.Perch >= 0) SetGoalToPerch(Beast, Where);
        if (!Flies(Beast, 40.0f, Seconds)) break;
        Beast.Rise = 0;
        Begin(Beast, Beast.Perch >= 0 ? Act::Perch : Act::Peck, Where.Chance.Between(2.0f, 6.0f));
        if (Beast.Doing == Act::Peck) Wanders(Beast, Where, 14.0f);
        break;
    case Act::Perch:
        Sits(Beast, Where);
        if (Beast.Left <= 0) BirdNext(Beast, Where);
        break;
    case Act::Sing:
        Sits(Beast, Where);
        if (std::fmod(Beast.Time, 1.1f) < Seconds) Where.Air.Note(Beast.X + (Beast.FacesLeft ? -4.0f : 4.0f), Beast.Y - 7.0f);
        if (Beast.Left <= 0) Begin(Beast, Act::Perch, Where.Chance.Between(1.0f, 3.0f));
        break;
    case Act::Peck:
        if (Walks(Beast, Where, 11.0f, Seconds)) {
            if (Where.Chance.Chance(0.5f)) Wanders(Beast, Where, 14.0f);
            else Beast.Left = std::min(Beast.Left, 0.0f);
        }
        if (Beast.Left <= 0) Seeks(Beast, Where, false);
        break;
    default:
        BirdNext(Beast, Where);
        break;
    }
}

// ---- the owl ------------------------------------------------------------------

void StepOwl(Animal &Beast, const Habitat &Where, float Seconds)
{
    Beast.Left -= Seconds;
    if (Comings(Beast, Where, Seconds, true, 30.0f)) return;
    switch (Beast.Doing) {
    case Act::Fly:
        SetGoalToPerch(Beast, Where);
        if (!Flies(Beast, 30.0f, Seconds)) break;
        Beast.Rise = 0;
        Begin(Beast, Act::Perch, Where.Chance.Between(6.0f, 14.0f));
        break;
    case Act::Perch:
        Sits(Beast, Where);
        if (Beast.Left > 0) break;
        if (Where.Chance.Chance(0.55f)) {
            Begin(Beast, Act::Hoot, 2.2f);
        } else {
            Seeks(Beast, Where, true);
        }
        break;
    case Act::Hoot:
        Sits(Beast, Where);
        if (std::fmod(Beast.Time, 1.0f) < Seconds) Where.Air.Note(Beast.X, Beast.Y - 11.0f);
        if (Beast.Left <= 0) Begin(Beast, Act::Perch, Where.Chance.Between(5.0f, 12.0f));
        break;
    default:
        Begin(Beast, Act::Perch, 4.0f);
        break;
    }
}

// ---- what only drifts ---------------------------------------------------------

namespace {

// The air and the meadow, between the treetops and the grass.
float Low(const Habitat &Where) { return static_cast<float>(Where.Place.Size.Front); }

void FadeIn(Animal &Drifter, const Habitat &Where, float Seconds)
{
    float Goal = !Drifter.Retire && Wants(Drifter, Where) ? 1.0f : 0.0f;
    Drifter.Presence = Ease(Drifter.Presence, Goal, 0.5f, Seconds);
}

void KeepOnGlass(Animal &Drifter, const Habitat &Where, float Top, float Bottom)
{
    Drifter.X = std::clamp(Drifter.X, -4.0f, Wide(Where) + 4.0f);
    Drifter.Y = std::clamp(Drifter.Y, Top, Bottom);
}

}  // namespace

void StepBat(Animal &Beast, const Habitat &Where, float Seconds)
{
    FadeIn(Beast, Where, Seconds);
    if (Beast.Presence < 0.02f) return;
    Beast.Left -= Seconds;
    if (Beast.Left <= 0) {
        Beast.GoalX = Beast.X + Where.Chance.Between(-45.0f, 45.0f);
        Beast.GoalY = Where.Chance.Between(5.0f, static_cast<float>(Where.Place.Size.Ground) * 0.85f);
        Beast.Left  = Where.Chance.Between(0.3f, 1.1f);
    }
    float Dx = Beast.GoalX - Beast.X, Dy = Beast.GoalY - Beast.Y;
    float Away = std::max(1.0f, std::sqrt(Dx * Dx + Dy * Dy));
    Beast.Vx += (Dx / Away * 34.0f - Beast.Vx) * std::min(1.0f, Seconds * 4.0f);
    Beast.Vy += (Dy / Away * 34.0f + Wave(Beast.Time * 2.3f + Beast.Phase) * 22.0f - Beast.Vy) * std::min(1.0f, Seconds * 4.0f);
    Beast.X += Beast.Vx * Seconds;
    Beast.Y += Beast.Vy * Seconds;
    Beast.FacesLeft = Beast.Vx < 0;
    KeepOnGlass(Beast, Where, 3.0f, static_cast<float>(Where.Place.Size.Ground));
}

void StepButterfly(Animal &Beast, const Habitat &Where, float Seconds)
{
    FadeIn(Beast, Where, Seconds);
    if (Beast.Presence < 0.02f) return;
    Beast.Left -= Seconds;
    if (Beast.Doing == Act::Sit) {
        if (Beast.Left <= 0) Begin(Beast, Act::Fly, 0);
        return;
    }
    Beast.Doing = Act::Fly;
    if (Beast.Left <= 0) {
        const std::vector<Flower> &Flowers = Where.Place.Flowers;
        const Flower &Pick = Flowers[static_cast<std::size_t>(Where.Chance.Count(0, static_cast<int>(Flowers.size()) - 1))];
        Beast.GoalX = static_cast<float>(Pick.X);
        Beast.GoalY = static_cast<float>(Pick.Y) - 2.0f;
        Beast.Left  = 12.0f;
    }
    float Dx = Beast.GoalX - Beast.X, Dy = Beast.GoalY - Beast.Y;
    float Away = std::sqrt(Dx * Dx + Dy * Dy);
    if (Away < 1.5f) {
        Begin(Beast, Act::Sit, Where.Chance.Between(2.0f, 5.0f));
        return;
    }
    float Step = std::min(13.0f * Seconds, Away);
    Beast.X += Dx / Away * Step;
    Beast.Y += Dy / Away * Step + Wave(Beast.Time * 1.4f + Beast.Phase) * 9.0f * Seconds;
    Beast.FacesLeft = Dx < 0;
    KeepOnGlass(Beast, Where, 3.0f, Low(Where));
}

void StepFirefly(Animal &Beast, const Habitat &Where, float Seconds)
{
    FadeIn(Beast, Where, Seconds);
    if (Beast.Presence < 0.02f) return;
    Beast.Vx += Where.Chance.Between(-1.0f, 1.0f) * 30.0f * Seconds;
    Beast.Vy += Where.Chance.Between(-1.0f, 1.0f) * 30.0f * Seconds;
    Beast.Vx = std::clamp(Beast.Vx, -5.0f, 5.0f);
    Beast.Vy = std::clamp(Beast.Vy, -3.0f, 3.0f);
    Beast.X += Beast.Vx * Seconds;
    Beast.Y += Beast.Vy * Seconds;
    float Top = static_cast<float>(Where.Place.Size.Ground) * 0.7f;
    if (Beast.Y < Top || Beast.Y > Low(Where) - 1.0f) Beast.Vy = -Beast.Vy;
    if (Beast.X < 0 || Beast.X > Wide(Where)) Beast.Vx = -Beast.Vx;
    KeepOnGlass(Beast, Where, Top, Low(Where) - 1.0f);
}

}  // namespace forest
