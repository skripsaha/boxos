/* fauna.cpp — the herd, and what every kind of animal shares: when it is its
 * hour, where it walks, how it comes in at the edge and goes out again. */

#include "fauna.h"

#include <algorithm>
#include <cmath>

namespace forest {

namespace {

constexpr float EdgeOutside = 7.0f;

// How fast each kind goes when it only means to get somewhere, and how far one
// turn of its legs takes it.
struct Pace {
    float Speed;
    float Stride;
};

Pace PaceOf(Kind Which)
{
    switch (Which) {
    case Kind::Fox:      return {17.0f, 8.0f};
    case Kind::Hedgehog: return {6.0f, 4.0f};
    case Kind::Hare:     return {34.0f, 10.0f};
    case Kind::Deer:     return {13.0f, 9.0f};
    default:             return {20.0f, 6.0f};
    }
}

bool Walker(Kind Which)
{
    return Which == Kind::Fox || Which == Kind::Hedgehog || Which == Kind::Hare || Which == Kind::Deer;
}

bool Drifter(Kind Which)
{
    return Which == Kind::Bat || Which == Kind::Butterfly || Which == Kind::Firefly;
}

Animal Born(Kind Which, const Habitat &Where, Dice &Chance)
{
    Animal Newborn;
    Newborn.What     = Which;
    Newborn.Phase    = Chance.Unit();
    Newborn.Variant  = Which == Kind::Deer ? Chance.Count(0, 2) : Chance.Count(0, 3);
    Newborn.Seen     = Where.Air.Strikes;
    Newborn.Left     = Chance.Between(0.5f, 8.0f);
    Newborn.FacesLeft = Chance.Chance(0.5f);
    if (Drifter(Which)) {
        const Layout &Size = Where.Place.Size;
        Newborn.X = Chance.Between(0.0f, static_cast<float>(Size.Width));
        Newborn.Y = Chance.Between(static_cast<float>(Size.Ground) * 0.5f, static_cast<float>(Size.Front));
    }
    return Newborn;
}

// How many of a kind a glass holds: the setting is for 128 x 96 pixels or
// more, a smaller glass holds fewer, and a kind that is wanted at all is always
// there.
std::size_t Room(std::uint32_t Wanted, const Layout &Size)
{
    float Area = static_cast<float>(Size.Width) * static_cast<float>(Size.Height) / (128.0f * 96.0f);
    return Wanted == 0 ? 0 : static_cast<std::size_t>(std::ceil(static_cast<float>(Wanted) * std::clamp(Area, 0.4f, 1.0f)));
}

// Standing about the glass already, at the first look.
void Stand(Animal &Beast, const Habitat &Where)
{
    const Layout &Size = Where.Place.Size;
    Dice         &Chance = Where.Chance;
    if (Drifter(Beast.What)) {
        Beast.Presence = Wants(Beast, Where) ? 1.0f : 0.0f;
        return;
    }
    if (!Wants(Beast, Where)) return;
    if (Walker(Beast.What)) {
        Beast.X = Chance.Between(6.0f, static_cast<float>(Size.Width) - 6.0f);
        Beast.Y = LaneAt(Size, Chance.Unit());
        Begin(Beast, Act::Sit, Chance.Between(0.5f, 4.0f));
        return;
    }
    Beast.Doing = Act::Away;
    Beast.Left  = Chance.Between(0.1f, 2.0f);
}

}  // namespace

bool Wants(const Animal &Beast, const Habitat &Where)
{
    const Air  &Aloft = Where.Weather.Aloft;
    const float Day   = Where.Sky.Day;
    const bool  Warm  = Where.Weather.CurrentSeason() != Season::Winter;
    switch (Beast.What) {
    case Kind::Fox:       return Aloft.Storm < 0.5f;
    case Kind::Hedgehog:  return !Warm || ((Day < 0.6f || Aloft.Rain > 0.4f) && Aloft.Storm < 0.7f);
    case Kind::Hare:      return Aloft.Storm < 0.6f;
    case Kind::Deer:      return Day > 0.12f && Aloft.Storm < 0.6f;
    case Kind::Bird:      return Day > 0.18f && Aloft.Rain < 0.55f && Aloft.Storm < 0.3f && (Warm || Beast.Variant % 2 == 0);
    case Kind::Owl:       return Day < 0.45f && Aloft.Storm < 0.6f;
    case Kind::Bat:       return Day < 0.35f && Warm && Aloft.Rain < 0.3f && Aloft.Wind < 0.8f;
    case Kind::Butterfly: return Day > 0.5f && Where.Weather.Ground.Flowers > 0.3f && Aloft.Rain < 0.15f && Aloft.Wind < 0.7f &&
                                 Aloft.Fog < 0.6f && Aloft.Gloom < 0.5f;
    case Kind::Firefly:   return Day < 0.3f && Where.Weather.Ground.Leaves > 0.7f && Where.Weather.Ground.Litter < 0.5f &&
                                 Where.Weather.Ground.Snow < 0.1f && Aloft.Rain < 0.3f;
    }
    return false;
}

float LaneAt(const Layout &Size, float Depth)
{
    return static_cast<float>(Size.Ground + 3) + Depth * static_cast<float>(Size.Front - Size.Ground - 3);
}

bool Walks(Animal &Beast, const Habitat &Where, float Speed, float Seconds)
{
    float Dx   = Beast.GoalX - Beast.X;
    float Dy   = Beast.GoalY - Beast.Y;
    float Away = std::sqrt(Dx * Dx + Dy * Dy);
    if (Away < 1.5f) return true;
    float Step = std::min(Speed * Seconds, Away);
    Beast.X += Dx / Away * Step;
    Beast.Y += Dy / Away * Step * 0.5f;
    Beast.Y = std::clamp(Beast.Y, LaneAt(Where.Place.Size, 0.0f), LaneAt(Where.Place.Size, 1.0f));
    Beast.Stride += Step / PaceOf(Beast.What).Stride;
    if (Dx < -0.3f) Beast.FacesLeft = true;
    if (Dx > 0.3f) Beast.FacesLeft = false;
    return false;
}

void Wanders(Animal &Beast, const Habitat &Where, float Reach)
{
    const Layout &Size = Where.Place.Size;
    Dice         &Chance = Where.Chance;
    float Wide   = static_cast<float>(Size.Width);
    Beast.GoalX  = std::clamp(Beast.X + Chance.Between(-Reach, Reach), 5.0f, Wide - 5.0f);
    float Depth  = (Beast.Y - LaneAt(Size, 0.0f)) / (LaneAt(Size, 1.0f) - LaneAt(Size, 0.0f));
    Beast.GoalY  = LaneAt(Size, std::clamp(Depth + Chance.Between(-0.25f, 0.25f), 0.0f, 1.0f));
}

void Begin(Animal &Beast, Act Doing, float Lasts)
{
    Beast.Doing = Doing;
    Beast.Left  = Lasts;
    Beast.Time  = 0;
    Beast.Rise  = 0;
}

const Animal *Near(const Fauna &Herd, Kind Which, float X, float Y, float Reach)
{
    const Animal *Best     = nullptr;
    float         BestAway = Reach;
    for (const Animal &Other : Herd.Animals) {
        if (Other.What != Which || Other.Doing == Act::Away) continue;
        float Away = std::hypot(Other.X - X, (Other.Y - Y) * 2.0f);
        if (Away < BestAway) {
            BestAway = Away;
            Best     = &Other;
        }
    }
    return Best;
}

bool Startled(Animal &Beast, const Habitat &Where)
{
    if (Where.Air.Strikes <= Beast.Seen) return false;
    Beast.Seen = Where.Air.Strikes;
    return true;
}

std::size_t Fauna::Living() const
{
    return static_cast<std::size_t>(std::count_if(Animals.begin(), Animals.end(), [](const Animal &Each) {
        return !Each.Retire && (Drifter(Each.What) ? Each.Presence > 0.5f : Each.Doing != Act::Away);
    }));
}

void Fauna::Fit(const Look &Shown, const Habitat &Where, bool Settled)
{
    for (int At = 0; At < KindCount; At++) {
        auto Which = static_cast<Kind>(At);
        std::size_t Have = static_cast<std::size_t>(
            std::count_if(Animals.begin(), Animals.end(), [Which](const Animal &Each) { return Each.What == Which && !Each.Retire; }));
        std::size_t Want = Room(Shown.Animals[At], Where.Place.Size);
        for (Animal &Each : Animals)
            if (Have < Want && Each.What == Which && Each.Retire) {
                Each.Retire = false;
                Have++;
            }
        for (; Have < Want; Have++) {
            Animals.push_back(Born(Which, Where, Where.Chance));
            if (Settled) Stand(Animals.back(), Where);
        }
        for (Animal &Each : Animals)
            if (Have > Want && Each.What == Which && !Each.Retire) {
                Each.Retire = true;
                Have--;
            }
    }
}

namespace {

// Away: waiting for its hour, then in at the edge. Leaving: on its way out.
// True while the routine has the animal, and the kind's own step is not wanted.
bool Routine(Animal &Beast, const Habitat &Where, float Seconds)
{
    const Layout &Size = Where.Place.Size;
    Dice         &Chance = Where.Chance;
    float Wide = static_cast<float>(Size.Width);

    if (Beast.Doing == Act::Away) {
        Beast.Left -= Seconds;
        if (Beast.Left > 0 || Beast.Retire || !Wants(Beast, Where)) return true;
        bool FromLeft = Chance.Chance(0.5f);
        Beast.X       = FromLeft ? -EdgeOutside : Wide + EdgeOutside;
        Beast.Y       = LaneAt(Size, Chance.Unit());
        Beast.GoalX   = FromLeft ? Chance.Between(8.0f, Wide * 0.4f) : Chance.Between(Wide * 0.6f, Wide - 8.0f);
        Beast.GoalY   = Beast.Y;
        Beast.Leaving = false;
        Begin(Beast, Beast.What == Kind::Hare ? Act::Hop : Act::Walk, 0);
        return true;
    }
    if (!Beast.Leaving && (Beast.Retire || !Wants(Beast, Where))) {
        Beast.Leaving = true;
        Beast.GoalX   = Beast.X < Wide / 2.0f ? -EdgeOutside - 2.0f : Wide + EdgeOutside + 2.0f;
        Beast.GoalY   = Beast.Y;
        Beast.Rise = 0;
    }
    if (!Beast.Leaving) return false;
    Beast.Doing = Beast.What == Kind::Hare ? Act::Hop : Act::Walk;
    float Speed = PaceOf(Beast.What).Speed * 1.2f;
    if (Beast.What == Kind::Hare ? Hops(Beast, Where, Speed, Seconds) : Walks(Beast, Where, Speed, Seconds)) {
        Beast.Leaving = false;
        Begin(Beast, Act::Away, Chance.Between(4.0f, 20.0f));
    }
    return true;
}

}  // namespace

void Fauna::Step(const Habitat &Where, float Seconds)
{
    for (std::size_t At = 0; At < Animals.size();) {
        Animal &Each = Animals[At];
        Each.Time += Seconds;
        if (Each.Retire && Each.Doing == Act::Away && !Drifter(Each.What)) {
            Animals[At] = Animals.back();
            Animals.pop_back();
            continue;
        }
        if (Each.Retire && Drifter(Each.What) && Each.Presence <= 0.01f) {
            Animals[At] = Animals.back();
            Animals.pop_back();
            continue;
        }
        if (Walker(Each.What) && Routine(Each, Where, Seconds)) {
            At++;
            continue;
        }
        switch (Each.What) {
        case Kind::Fox:       StepFox(Each, *this, Where, Seconds); break;
        case Kind::Hedgehog:  StepHedgehog(Each, *this, Where, Seconds); break;
        case Kind::Hare:      StepHare(Each, *this, Where, Seconds); break;
        case Kind::Deer:      StepDeer(Each, *this, Where, Seconds); break;
        case Kind::Bird:      StepBird(Each, Where, Seconds); break;
        case Kind::Owl:       StepOwl(Each, Where, Seconds); break;
        case Kind::Bat:       StepBat(Each, Where, Seconds); break;
        case Kind::Butterfly: StepButterfly(Each, Where, Seconds); break;
        case Kind::Firefly:   StepFirefly(Each, Where, Seconds); break;
        }
        At++;
    }
}

}  // namespace forest
