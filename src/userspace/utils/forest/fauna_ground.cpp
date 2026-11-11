/* fauna_ground.cpp — the lives of the fox, the hedgehog, the hare and the
 * deer: what each does when nothing is the matter, and what it does when
 * something is. */

#include <algorithm>
#include <cmath>

#include "fauna.h"

namespace forest {

namespace {

constexpr float Pi = 3.14159265358979323846f;

float Wide(const Habitat &Where) { return static_cast<float>(Where.Place.Size.Width); }

float FarSide(const Animal &Beast, const Habitat &Where)
{
    return Beast.X < Wide(Where) / 2.0f ? Wide(Where) - 8.0f : 8.0f;
}

// Away from a thing at `FromX`, a good way off and on the glass.
void Flees(Animal &Beast, const Habitat &Where, float FromX)
{
    float Way   = Beast.X >= FromX ? 1.0f : -1.0f;
    Beast.GoalX = std::clamp(Beast.X + Way * Where.Chance.Between(45.0f, 80.0f), 6.0f, Wide(Where) - 6.0f);
    if (std::fabs(Beast.GoalX - Beast.X) < 15.0f) Beast.GoalX = FarSide(Beast, Where);
    Beast.GoalY = LaneAt(Where.Place.Size, Where.Chance.Unit());
}

// The foot of the bush nearest X: where a hedgehog beds down for the winter.
void HomeNear(const Habitat &Where, float X, float &HomeX, float &HomeFoot)
{
    HomeX    = X;
    HomeFoot = LaneAt(Where.Place.Size, 0.5f);
    float Best = 1e9f;
    for (const Bush &Each : Where.Place.Bushes) {
        if (std::fabs(Each.X - X) >= Best) continue;
        Best     = std::fabs(Each.X - X);
        HomeX    = Each.X;
        HomeFoot = Each.Foot + 1.0f;
    }
}

// ---- the fox ------------------------------------------------------------------

void FoxNext(Animal &Fox, const Habitat &Where)
{
    float Roll = Where.Chance.Unit();
    Dice &Chance = Where.Chance;
    if (Roll < 0.40f) {
        Wanders(Fox, Where, 60.0f);
        Begin(Fox, Act::Trot, 0);
    } else if (Roll < 0.64f) {
        Begin(Fox, Act::Sniff, Chance.Between(1.5f, 3.5f));
    } else if (Roll < 0.80f) {
        Begin(Fox, Act::Sit, Chance.Between(3.0f, 7.0f));
    } else if (Roll < 0.90f && Where.Sky.Day > 0.25f && Where.Weather.Aloft.Rain < 0.3f) {
        Begin(Fox, Act::Sleep, Chance.Between(10.0f, 18.0f));
    } else {
        Wanders(Fox, Where, 40.0f);
        Begin(Fox, Act::Trot, 0);
    }
}

// The leap of a fox that has heard a mouse under the snow: up and over, nose
// first into the drift, a puff of powder, and a moment with its tail in the air.
void FoxPounce(Animal &Fox, const Habitat &Where, float Seconds)
{
    constexpr float Flight = 0.45f, Buried = 1.4f;
    float Way = Fox.FacesLeft ? -1.0f : 1.0f;
    if (Fox.Time < Flight) {
        Fox.Rise = 6.5f * std::sin(Pi * Fox.Time / Flight);
        Fox.X += Way * 11.0f / Flight * Seconds;
        return;
    }
    if (Fox.Rise > 0) {
        Fox.Rise = 0;
        Where.Air.Powder(Fox.X + Way * 6.0f, Fox.Y - 2.0f, 9, Where.Chance);
    }
    if (Fox.Time >= Buried) {
        Wanders(Fox, Where, 50.0f);
        Begin(Fox, Act::Trot, 0);
    }
}

}  // namespace

void StepFox(Animal &Beast, const Fauna &, const Habitat &Where, float Seconds)
{
    Beast.Left -= Seconds;
    Beast.Fear -= Seconds;
    if (Startled(Beast, Where)) {
        Beast.Fear = 2.5f;
        Beast.GoalX = FarSide(Beast, Where);
        Beast.GoalY = LaneAt(Where.Place.Size, Where.Chance.Unit());
        Begin(Beast, Act::Run, 3.0f);
    }
    bool InSnow = Where.Weather.Ground.Snow > 0.4f;
    switch (Beast.Doing) {
    case Act::Walk:
    case Act::Trot:
        if (Walks(Beast, Where, 17.0f, Seconds)) FoxNext(Beast, Where);
        break;
    case Act::Run:
        if (Walks(Beast, Where, 40.0f, Seconds) || Beast.Left <= 0) FoxNext(Beast, Where);
        break;
    case Act::Sniff:
        if (Beast.Left > 0) break;
        if (InSnow && Where.Chance.Chance(0.55f)) Begin(Beast, Act::Pounce, 1.4f);
        else FoxNext(Beast, Where);
        break;
    case Act::Pounce:
        FoxPounce(Beast, Where, Seconds);
        break;
    case Act::Sleep:
        if (Beast.Left <= 0) Begin(Beast, Act::Sit, 1.2f);
        break;
    case Act::Sit:
        if (Beast.Left <= 0) FoxNext(Beast, Where);
        break;
    default:
        FoxNext(Beast, Where);
        break;
    }
}

// ---- the hedgehog -------------------------------------------------------------

namespace {

void HedgehogNext(Animal &Hog, const Habitat &Where)
{
    float Roll = Where.Chance.Unit();
    if (Roll < 0.55f) {
        Wanders(Hog, Where, 28.0f);
        Begin(Hog, Act::Walk, 0);
    } else {
        Begin(Hog, Act::Sniff, Where.Chance.Between(2.0f, 5.0f));
    }
}

// All winter in a bed of leaves at the foot of a bush.
void Hibernate(Animal &Hog, const Habitat &Where, float Seconds)
{
    if (Hog.Doing == Act::Sleep) return;
    float HomeX, HomeFoot;
    HomeNear(Where, Hog.X, HomeX, HomeFoot);
    Hog.GoalX  = HomeX;
    Hog.GoalY  = HomeFoot;
    Hog.Doing  = Act::Walk;
    if (Walks(Hog, Where, 7.0f, Seconds)) Begin(Hog, Act::Sleep, 1e9f);
}

}  // namespace

void StepHedgehog(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds)
{
    if (Where.Weather.CurrentSeason() == Season::Winter) {
        Hibernate(Beast, Where, Seconds);
        return;
    }
    if (Beast.Doing == Act::Sleep && Beast.Left > 1000.0f) Begin(Beast, Act::Sleep, 2.5f);
    Beast.Left -= Seconds;
    Beast.Fear -= Seconds;
    if (Startled(Beast, Where)) Beast.Fear = 5.0f;
    const Animal *Fox = Near(Herd, Kind::Fox, Beast.X, Beast.Y, 30.0f);
    if (Fox && Fox->Doing != Act::Sit && Fox->Doing != Act::Sleep) Beast.Fear = 4.0f;
    if (Beast.Fear > 0 && Beast.Doing != Act::Curl) Begin(Beast, Act::Curl, 5.5f);

    switch (Beast.Doing) {
    case Act::Walk:
        if (Walks(Beast, Where, 6.0f, Seconds)) HedgehogNext(Beast, Where);
        break;
    case Act::Curl:
        if (Beast.Left <= 0 && Beast.Fear <= 0) Begin(Beast, Act::Sniff, 1.2f);
        break;
    case Act::Sniff:
    case Act::Sleep:
        if (Beast.Left <= 0) HedgehogNext(Beast, Where);
        break;
    default:
        HedgehogNext(Beast, Where);
        break;
    }
}

// ---- the hare -----------------------------------------------------------------

bool Hops(Animal &Beast, const Habitat &Where, float Speed, float Seconds)
{
    float Period = Beast.Fear > 0 ? 0.34f : 0.46f;
    float Cycle  = std::fmod(Beast.Time, Period) / Period;
    float Dx     = Beast.GoalX - Beast.X;
    float Dy     = Beast.GoalY - Beast.Y;
    float Away   = std::sqrt(Dx * Dx + Dy * Dy);
    if (Away < 1.5f) {
        Beast.Rise = 0;
        return true;
    }
    if (Dx < -0.3f) Beast.FacesLeft = true;
    if (Dx > 0.3f) Beast.FacesLeft = false;
    constexpr float Takeoff = 0.2f, Landing = 0.8f;
    if (Cycle < Takeoff || Cycle > Landing) {
        Beast.Rise = 0;
        return false;
    }
    float Step = std::min(Speed / (Landing - Takeoff) * Seconds, Away);
    Beast.X += Dx / Away * Step;
    Beast.Y = std::clamp(Beast.Y + Dy / Away * Step * 0.5f, LaneAt(Where.Place.Size, 0.0f), LaneAt(Where.Place.Size, 1.0f));
    Beast.Rise = std::sin(Pi * (Cycle - Takeoff) / (Landing - Takeoff)) * (Beast.Fear > 0 ? 6.0f : 4.2f);
    return false;
}

namespace {

void HareNext(Animal &Hare, const Habitat &Where)
{
    float Roll = Where.Chance.Unit();
    Dice &Chance = Where.Chance;
    if (Roll < 0.45f) {
        Wanders(Hare, Where, 40.0f);
        Begin(Hare, Act::Hop, 0);
    } else if (Roll < 0.65f) {
        Begin(Hare, Act::Wash, Chance.Between(2.0f, 3.5f));
    } else if (Roll < 0.80f) {
        Begin(Hare, Act::Alert, Chance.Between(1.2f, 2.2f));
    } else {
        Begin(Hare, Act::Sit, Chance.Between(1.5f, 5.0f));
    }
}

}  // namespace

void StepHare(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds)
{
    Beast.Left -= Seconds;
    Beast.Fear -= Seconds;
    const Animal *Fox = Near(Herd, Kind::Fox, Beast.X, Beast.Y, 48.0f);
    if (Beast.Fear <= 0 && Fox) {
        Beast.Fear = 2.2f;
        Flees(Beast, Where, Fox->X);
        Begin(Beast, Act::Hop, 0);
    }
    if (Startled(Beast, Where)) {
        Beast.Fear = 2.5f;
        Flees(Beast, Where, Beast.X + Where.Chance.Between(-10.0f, 10.0f));
        Begin(Beast, Act::Hop, 0);
    }
    switch (Beast.Doing) {
    case Act::Hop:
        if (!Hops(Beast, Where, Beast.Fear > 0 ? 62.0f : 26.0f, Seconds)) break;
        if (Beast.Fear > 0) Begin(Beast, Act::Alert, 1.4f);
        else HareNext(Beast, Where);
        break;
    case Act::Wash:
        if (Beast.Left <= 0) Begin(Beast, Act::Sit, 1.0f);
        break;
    case Act::Alert:
    case Act::Sit:
        if (Beast.Left <= 0) HareNext(Beast, Where);
        break;
    default:
        HareNext(Beast, Where);
        break;
    }
}

// ---- the deer -----------------------------------------------------------------

namespace {

constexpr int Doe = 1, Fawn = 2;

// A fawn keeps to its mother.
const Animal *MotherOf(const Fauna &Herd, const Animal &Young)
{
    const Animal *Best = nullptr;
    float         BestAway = 1e9f;
    for (const Animal &Other : Herd.Animals) {
        if (Other.What != Kind::Deer || Other.Variant != Doe || Other.Doing == Act::Away) continue;
        float Away = std::fabs(Other.X - Young.X);
        if (Away < BestAway) {
            BestAway = Away;
            Best     = &Other;
        }
    }
    return Best;
}

void DeerNext(Animal &Deer, const Fauna &Herd, const Habitat &Where)
{
    Dice &Chance = Where.Chance;
    if (Deer.Variant == Fawn)
        if (const Animal *Mother = MotherOf(Herd, Deer)) {
            Deer.GoalX = std::clamp(Mother->X + Chance.Between(-24.0f, 24.0f), 5.0f, Wide(Where) - 5.0f);
            Deer.GoalY = std::clamp(Mother->Y + Chance.Between(-4.0f, 4.0f), LaneAt(Where.Place.Size, 0.0f),
                                    LaneAt(Where.Place.Size, 1.0f));
            Begin(Deer, Chance.Chance(0.35f) ? Act::Sit : Act::Walk, Chance.Between(1.5f, 4.0f));
            return;
        }
    float Roll = Chance.Unit();
    if (Roll < 0.40f) {
        Begin(Deer, Act::Graze, Chance.Between(4.0f, 9.0f));
    } else if (Roll < 0.65f) {
        Wanders(Deer, Where, 40.0f);
        Begin(Deer, Act::Walk, 0);
    } else if (Roll < 0.80f) {
        Begin(Deer, Act::Alert, Chance.Between(2.0f, 3.5f));
    } else if (Roll < 0.88f) {
        Begin(Deer, Act::Lie, Chance.Between(8.0f, 14.0f));
    } else {
        Wanders(Deer, Where, 50.0f);
        Begin(Deer, Act::Walk, 0);
    }
}

}  // namespace

void StepDeer(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds)
{
    Beast.Left -= Seconds;
    Beast.Fear -= Seconds;
    if (Startled(Beast, Where)) {
        Beast.Fear = 3.0f;
        Begin(Beast, Act::Alert, 0.7f);
    }
    switch (Beast.Doing) {
    case Act::Walk:
        if (Walks(Beast, Where, Beast.Variant == Fawn ? 15.0f : 11.0f, Seconds)) DeerNext(Beast, Herd, Where);
        break;
    case Act::Run:
        if (Walks(Beast, Where, 42.0f, Seconds)) Begin(Beast, Act::Alert, 2.0f);
        break;
    case Act::Alert:
        if (Beast.Left > 0) break;
        if (Beast.Fear > 0) {
            Beast.GoalX = FarSide(Beast, Where);
            Beast.GoalY = LaneAt(Where.Place.Size, Where.Chance.Unit());
            Begin(Beast, Act::Run, 0);
        } else {
            DeerNext(Beast, Herd, Where);
        }
        break;
    case Act::Graze:
    case Act::Lie:
    case Act::Sit:
        if (Beast.Left <= 0) DeerNext(Beast, Herd, Where);
        break;
    default:
        DeerNext(Beast, Herd, Where);
        break;
    }
}

}  // namespace forest
