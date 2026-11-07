/* aquarium_life.cpp — how what lives in the tank moves.
 *
 * A swimmer steers: it picks somewhere to go in its band of depth and turns
 * toward it a little at a time, so nothing ever jumps. Hunger grows; a hungry
 * one goes for the nearest flake it can see, the crab and the snail for what
 * has sunk to the sand. A knock on the glass sends what swims in front of it
 * away, the pufferfish blown up; the crab and the snail keep still until it is
 * quiet again. */

#include <algorithm>
#include <cmath>

#include "aquarium.h"

namespace {

constexpr float HungerSeconds = 90.0f;   /* from fed to hungry */
constexpr float HungryFrom    = 0.4f;
constexpr float SightPixels   = 70.0f;
constexpr float FlakeSink     = 7.0f;    /* pixels a second */
constexpr float FlakeLasts    = 25.0f;   /* seconds on the sand */
constexpr float BubblerEvery  = 0.35f;
constexpr float KnockReach    = 80.0f;

const AquaColor FlakeInks[] = {AquaColor::Rgb(0xE0, 0xA0, 0x40), AquaColor::Rgb(0xC0, 0x60, 0x30), AquaColor::Rgb(0xE8, 0xD0, 0x80)};

const AquaKind &KindOf(const AquaCreature &Creature)
{
    return AquaKinds()[static_cast<std::size_t>(Creature.Kind)];
}

float Length(float X, float Y)
{
    return std::sqrt(X * X + Y * Y);
}

/* Where it eats: the middle of its head. */
void Mouth(const AquaCreature &Creature, float &X, float &Y)
{
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    X = Creature.X + (Creature.Left ? 0.0f : static_cast<float>(Sprite.Width - 1));
    Y = Creature.Y + static_cast<float>(Sprite.Height) / 2.0f;
}

/* The flake nearest the mouth that it can see: in the water, or on the sand. */
int NearestFlake(const AquaTank &Tank, const AquaCreature &Creature, bool OnSand)
{
    float X, Y;
    Mouth(Creature, X, Y);
    int   Best     = -1;
    float BestSeen = SightPixels;
    for (std::size_t I = 0; I < Tank.Flakes.size(); I++) {
        const AquaFlake &Flake = Tank.Flakes[I];
        if ((Flake.Lying > 0) != OnSand) continue;
        float Seen = Length(Flake.X - X, Flake.Y - Y);
        if (Seen < BestSeen) {
            BestSeen = Seen;
            Best     = static_cast<int>(I);
        }
    }
    return Best;
}

/* A flake under the mouth is eaten, and a bubble says so. */
bool Eat(AquaTank &Tank, AquaCreature &Creature, int Flake, AquaDice &Dice)
{
    float X, Y;
    Mouth(Creature, X, Y);
    const AquaFlake &Food = Tank.Flakes[static_cast<std::size_t>(Flake)];
    if (std::fabs(Food.X - X) > 2.0f || std::fabs(Food.Y - Y) > 2.0f) return false;
    Tank.Flakes.erase(Tank.Flakes.begin() + Flake);
    Creature.Hunger = 0;
    Tank.Bubbles.push_back({X, Y - 1, Dice.Between(8.0f, 12.0f), Dice.Unit(), false});
    return true;
}

void KeepInWater(const AquaTank &Tank, AquaCreature &Creature)
{
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    Creature.X = std::clamp(Creature.X, 0.0f, static_cast<float>(Tank.Width - Sprite.Width));
    float Top    = static_cast<float>(Tank.Surface + 1);
    float Bottom = static_cast<float>(AquaFloor(Tank, static_cast<int>(Creature.X) + Sprite.Width / 2) - Sprite.Height - 1);
    Creature.Y = std::clamp(Creature.Y, Top, std::max(Top, Bottom));
}

/* Toward the goal, turning a little each step; the stroke quickens with speed. */
void Steer(AquaTank &Tank, AquaCreature &Creature, float Speed, float Seconds)
{
    float Dx   = Creature.GoalX - Creature.X;
    float Dy   = Creature.GoalY - Creature.Y;
    float Away = Length(Dx, Dy);
    float WantX = 0, WantY = 0;
    if (Away > 0.5f) {
        float Ease = std::min(1.0f, Away / 6.0f);
        WantX = Dx / Away * Speed * Ease;
        WantY = Dy / Away * Speed * Ease * 0.6f;
    }
    float Turn = std::min(1.0f, Seconds * 1.5f);
    Creature.Vx += (WantX - Creature.Vx) * Turn;
    Creature.Vy += (WantY - Creature.Vy) * Turn;
    Creature.X  += Creature.Vx * Seconds;
    Creature.Y  += Creature.Vy * Seconds;
    if (Creature.Vx > 0.3f) Creature.Left = false;
    if (Creature.Vx < -0.3f) Creature.Left = true;
    float Pace = Speed > 0 ? Length(Creature.Vx, Creature.Vy) / Speed : 0;
    Creature.Stroke += Seconds * (0.6f + 1.4f * Pace);
    KeepInWater(Tank, Creature);
}

/* Somewhere else to go in its band, within a third of the tank. */
void Wander(const AquaTank &Tank, AquaCreature &Creature, const AquaKind &Kind, AquaDice &Dice)
{
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    float Reach   = static_cast<float>(Tank.Width) / 3.0f;
    Creature.GoalX = std::clamp(Creature.X + Dice.Between(-Reach, Reach), 0.0f,
                                static_cast<float>(Tank.Width - Sprite.Width));
    Creature.GoalY = AquaBandY(Tank, Dice.Between(Kind.Top, Kind.Bottom), Sprite.Height,
                               static_cast<int>(Creature.GoalX));
    Creature.GoalLeft = Dice.Between(4.0f, 10.0f);
}

/* A swimmer: frightened it flees, hungry it feeds, else it goes where it
 * meant to, behind its leader or by its anemone. */
void Swim(AquaTank &Tank, std::size_t Index, float Seconds, AquaDice &Dice)
{
    AquaCreature   &Creature = Tank.Creatures[Index];
    const AquaKind &Kind     = KindOf(Creature);
    float           Speed    = Kind.Speed;
    Creature.GoalLeft -= Seconds;

    if (Creature.Fright > 0) {
        Creature.Fright -= Seconds;
        Speed *= Kind.Puffed.empty() ? 2.5f : 0.3f;
    } else if (int Flake = Creature.Hunger > HungryFrom ? NearestFlake(Tank, Creature, false) : -1; Flake >= 0) {
        const AquaFlake  &Food   = Tank.Flakes[static_cast<std::size_t>(Flake)];
        const AquaSprite &Sprite = AquaFrameOf(Creature);
        Creature.GoalX = Food.X - (Creature.Left ? 0.0f : static_cast<float>(Sprite.Width - 1));
        Creature.GoalY = Food.Y - static_cast<float>(Sprite.Height) / 2.0f;
        Speed *= 1.6f;
        if (Eat(Tank, Creature, Flake, Dice)) return;
    } else if (Creature.Leader >= 0) {
        const AquaCreature &Leader = Tank.Creatures[static_cast<std::size_t>(Creature.Leader)];
        Creature.GoalX = Leader.X + (Leader.Left ? -Creature.OffsetX : Creature.OffsetX);
        Creature.GoalY = Leader.Y + Creature.OffsetY;
        Speed *= 1.3f;
    } else if (Kind.Way == AquaWay::Guards) {
        if (Creature.GoalLeft <= 0) {
            Creature.GoalX    = static_cast<float>(Tank.Scene.AnemoneX) + Dice.Between(-12.0f, 8.0f);
            Creature.GoalY    = static_cast<float>(Tank.Scene.AnemoneY) - Dice.Between(5.0f, 14.0f);
            Creature.GoalLeft = Dice.Between(2.0f, 5.0f);
        }
    } else if (Creature.GoalLeft <= 0 || Length(Creature.GoalX - Creature.X, Creature.GoalY - Creature.Y) < 2.0f) {
        Wander(Tank, Creature, Kind, Dice);
    }
    Steer(Tank, Creature, Speed, Seconds);
}

/* A jellyfish: a stroke lifts it, between strokes it sinks, and it drifts. */
void Drift(AquaTank &Tank, AquaCreature &Creature, float Seconds)
{
    const AquaKind   &Kind   = KindOf(Creature);
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    float High = AquaBandY(Tank, Kind.Top, Sprite.Height, static_cast<int>(Creature.X));
    float Low  = AquaBandY(Tank, Kind.Bottom, Sprite.Height, static_cast<int>(Creature.X));

    Creature.Stroke += Seconds * 0.45f;
    bool Pulling = AquaWave(Creature.Stroke) > 0.55f && Creature.Y > High;
    if (Creature.Y > Low) Pulling = true;
    Creature.Vy += (Pulling ? -9.0f : 1.6f) * Seconds;
    Creature.Vy  = std::clamp(Creature.Vy, -Kind.Speed * 2.0f, Kind.Speed * 0.6f);
    Creature.Vx  = Kind.Speed * 0.5f * AquaWave(Tank.Time * 0.03f + Creature.Phase);
    Creature.X  += Creature.Vx * Seconds;
    Creature.Y  += Creature.Vy * Seconds;
    KeepInWater(Tank, Creature);
}

/* A seahorse: upright by a plant, bobbing, turning to look about. */
void Hover(AquaTank &Tank, AquaCreature &Creature, float Seconds, AquaDice &Dice)
{
    const AquaKind &Kind = KindOf(Creature);
    Creature.GoalLeft -= Seconds;
    if (Creature.GoalLeft <= 0 && !Tank.Scene.Stalks.empty()) {
        const AquaStalk &Stalk = Tank.Scene.Stalks[static_cast<std::size_t>(
            Dice.Count(0, static_cast<int>(Tank.Scene.Stalks.size()) - 1))];
        if (std::fabs(static_cast<float>(Stalk.X) - Creature.X) < static_cast<float>(Tank.Width) / 4.0f) {
            Creature.GoalX = static_cast<float>(Stalk.X) + Dice.Between(-3.0f, 3.0f);
            Creature.GoalY = AquaBandY(Tank, Dice.Between(Kind.Top, Kind.Bottom), AquaFrameOf(Creature).Height,
                                       Stalk.X);
        }
        Creature.GoalLeft = Dice.Between(8.0f, 20.0f);
    }
    Creature.GoalY += AquaWave(Tank.Time * 0.25f + Creature.Phase) * Seconds * 2.0f;
    bool Looked = Creature.Left;
    Steer(Tank, Creature, Kind.Speed * (Creature.Fright > 0 ? 3.0f : 1.0f), Seconds);
    if (Creature.Fright > 0) Creature.Fright -= Seconds;
    if (std::fabs(Creature.Vx) < 0.6f) Creature.Left = Looked;
}

/* The crab and the snail: along the sand, stopping now and then; to a flake
 * that has sunk, if one is near. */
void Crawl(AquaTank &Tank, std::size_t Index, float Seconds, AquaDice &Dice)
{
    AquaCreature     &Creature = Tank.Creatures[Index];
    const AquaKind   &Kind     = KindOf(Creature);
    const AquaSprite &Sprite   = AquaFrameOf(Creature);

    if (Creature.Rest > 0) {
        Creature.Rest -= Seconds;
    } else {
        int Flake = NearestFlake(Tank, Creature, true);
        if (Flake >= 0) {
            Creature.GoalX = Tank.Flakes[static_cast<std::size_t>(Flake)].X - static_cast<float>(Sprite.Width) / 2.0f;
            if (Eat(Tank, Creature, Flake, Dice)) Creature.Rest = Dice.Between(1.0f, 3.0f);
        } else if (std::fabs(Creature.GoalX - Creature.X) < 1.0f) {
            Creature.Rest  = Dice.Between(1.0f, Kind.Way == AquaWay::Walks ? 4.0f : 10.0f);
            Creature.GoalX = std::clamp(Creature.X + Dice.Between(-40.0f, 40.0f), 0.0f,
                                        static_cast<float>(Tank.Width - Sprite.Width));
        }
        float Step = std::min(Kind.Speed * Seconds, std::fabs(Creature.GoalX - Creature.X));
        Creature.Left = Creature.GoalX < Creature.X;
        Creature.X   += Creature.Left ? -Step : Step;
        Creature.Stroke += Seconds * (Kind.Way == AquaWay::Walks ? 2.0f : 0.3f);
    }
    Creature.Y = static_cast<float>(AquaFloor(Tank, static_cast<int>(Creature.X) + Sprite.Width / 2) - Sprite.Height + 1);
}

/* A visitor crosses from one side to the other, and leaves. */
void Visit(AquaTank &Tank, AquaCreature &Creature, float Seconds)
{
    const AquaKind   &Kind   = KindOf(Creature);
    const AquaSprite &Sprite = AquaFrameOf(Creature);
    Creature.X      += (Creature.Left ? -Kind.Speed : Kind.Speed) * Seconds;
    Creature.Y      += AquaWave(Tank.Time * 0.08f) * Seconds * 1.5f;
    Creature.Stroke += Seconds * 0.5f;
    Creature.Gone    = Creature.X < static_cast<float>(-Sprite.Width) || Creature.X > static_cast<float>(Tank.Width);
}

void StepCreature(AquaTank &Tank, std::size_t Index, float Seconds, AquaDice &Dice)
{
    AquaCreature &Creature = Tank.Creatures[Index];
    Creature.Hunger = std::min(1.0f, Creature.Hunger + Seconds / HungerSeconds);
    switch (KindOf(Creature).Way) {
    case AquaWay::Swims:
    case AquaWay::Schools:
    case AquaWay::Guards: Swim(Tank, Index, Seconds, Dice); break;
    case AquaWay::Drifts: Drift(Tank, Creature, Seconds); break;
    case AquaWay::Hovers: Hover(Tank, Creature, Seconds, Dice); break;
    case AquaWay::Walks:
    case AquaWay::Creeps: Crawl(Tank, Index, Seconds, Dice); break;
    case AquaWay::Visits: Visit(Tank, Creature, Seconds); break;
    case AquaWay::Rests: break;
    }
}

/* Visitors that have left are let go; who schooled behind one that came
 * later than them keeps its leader. */
void LetGo(AquaTank &Tank)
{
    for (std::size_t Index = Tank.Creatures.size(); Index-- > 0;) {
        if (!Tank.Creatures[Index].Gone) continue;
        Tank.Creatures.erase(Tank.Creatures.begin() + static_cast<std::ptrdiff_t>(Index));
        for (AquaCreature &Creature : Tank.Creatures)
            if (Creature.Leader > static_cast<int>(Index)) Creature.Leader--;
    }
}

void StepFlakes(AquaTank &Tank, float Seconds)
{
    for (std::size_t I = Tank.Flakes.size(); I-- > 0;) {
        AquaFlake &Flake = Tank.Flakes[I];
        float      Floor = static_cast<float>(AquaFloor(Tank, static_cast<int>(Flake.X)));
        if (Flake.Y < Floor - 1) {
            Flake.Y += FlakeSink * Seconds;
            Flake.X += AquaWave(Tank.Time * 0.5f + Flake.Phase) * Seconds * 2.0f;
            Flake.X  = std::clamp(Flake.X, 0.0f, static_cast<float>(Tank.Width - 1));
        } else {
            Flake.Y      = Floor - 1;
            Flake.Lying += Seconds;
        }
        if (Flake.Lying > FlakeLasts) Tank.Flakes.erase(Tank.Flakes.begin() + static_cast<std::ptrdiff_t>(I));
    }
}

void StepBubbles(AquaTank &Tank, float Seconds, AquaDice &Dice)
{
    Tank.SinceBubble += Seconds;
    for (; Tank.SinceBubble >= BubblerEvery; Tank.SinceBubble -= BubblerEvery)
        Tank.Bubbles.push_back({static_cast<float>(Tank.Scene.BubblerX) + Dice.Between(-0.5f, 0.5f),
                                static_cast<float>(Tank.Scene.BubblerY - 2), Dice.Between(10.0f, 16.0f), Dice.Unit(),
                                Dice.Chance(0.2f)});

    for (std::size_t I = Tank.Bubbles.size(); I-- > 0;) {
        AquaBubble &Bubble = Tank.Bubbles[I];
        Bubble.Y -= Bubble.Rise * Seconds;
        Bubble.X += AquaWave(Tank.Time * 1.5f + Bubble.Phase) * Seconds * 3.0f;
        if (Bubble.Y <= static_cast<float>(Tank.Surface))
            Tank.Bubbles.erase(Tank.Bubbles.begin() + static_cast<std::ptrdiff_t>(I));
    }
}

/* The chest waits, opens, breathes out a burst of bubbles, and shuts. */
void StepChest(AquaTank &Tank, float Seconds, AquaDice &Dice)
{
    AquaChest &Chest = Tank.Scene.Chest;
    if (Chest.Wait > 0) {
        Chest.Wait -= Seconds;
        if (Chest.Wait <= 0) Chest.Held = Dice.Between(2.0f, 3.5f);
        Chest.Open = std::max(0.0f, Chest.Open - Seconds * 1.2f);
        return;
    }
    Chest.Open = std::min(1.0f, Chest.Open + Seconds * 1.6f);
    if (Chest.Open < 1.0f) return;
    if (Dice.Chance(Seconds * 8.0f))
        Tank.Bubbles.push_back({static_cast<float>(Chest.X + Dice.Count(2, 6)), static_cast<float>(Chest.Y - 1),
                                Dice.Between(12.0f, 20.0f), Dice.Unit(), Dice.Chance(0.4f)});
    Chest.Held -= Seconds;
    if (Chest.Held <= 0) Chest.Wait = Dice.Between(20.0f, 45.0f);
}

void StepVisitor(AquaTank &Tank, float Seconds, AquaDice &Dice)
{
    Tank.VisitorWait -= Seconds;
    if (Tank.VisitorWait > 0) return;
    Tank.VisitorWait = Dice.Between(120.0f, 300.0f);

    const std::vector<AquaKind> &Kinds = AquaKinds();
    for (int Index = 0; Index < static_cast<int>(Kinds.size()); Index++) {
        if (Kinds[static_cast<std::size_t>(Index)].Way != AquaWay::Visits) continue;
        bool         FromLeft = Dice.Chance(0.5f);
        AquaCreature Visitor  = AquaHatch(Tank, Index, 0, Dice);
        Visitor.Left = !FromLeft;
        Visitor.X    = FromLeft ? static_cast<float>(-Kinds[static_cast<std::size_t>(Index)].Frames[0].Width)
                                : static_cast<float>(Tank.Width);
        Tank.Creatures.push_back(Visitor);
        return;
    }
}

}

void AquaStep(AquaTank &Tank, float Seconds, AquaDice &Dice)
{
    Tank.Time += Seconds;
    StepFlakes(Tank, Seconds);
    for (std::size_t Index = 0; Index < Tank.Creatures.size(); Index++) StepCreature(Tank, Index, Seconds, Dice);
    LetGo(Tank);
    StepBubbles(Tank, Seconds, Dice);
    StepChest(Tank, Seconds, Dice);
    StepVisitor(Tank, Seconds, Dice);
}

void AquaFeed(AquaTank &Tank, float X, AquaDice &Dice)
{
    for (int Flake = Dice.Count(6, 9); Flake > 0; Flake--)
        Tank.Flakes.push_back({std::clamp(X + Dice.Between(-7.0f, 7.0f), 0.0f, static_cast<float>(Tank.Width - 1)),
                               static_cast<float>(Tank.Surface + 1) + Dice.Between(0.0f, 2.0f), Dice.Unit(), 0,
                               FlakeInks[static_cast<std::size_t>(Dice.Count(0, 2))]});
    Tank.Fed++;
}

void AquaTap(AquaTank &Tank, float X, float Y)
{
    for (AquaCreature &Creature : Tank.Creatures) {
        if (Length(Creature.X - X, Creature.Y - Y) > KnockReach) continue;
        const AquaKind &Kind = KindOf(Creature);
        switch (Kind.Way) {
        case AquaWay::Walks:
        case AquaWay::Creeps: Creature.Rest = 4.0f; break;
        case AquaWay::Swims:
        case AquaWay::Schools:
        case AquaWay::Guards:
        case AquaWay::Hovers:
            Creature.Fright   = Kind.Puffed.empty() ? 1.8f : 4.0f;
            Creature.GoalX    = std::clamp(Creature.X + (Creature.X < X ? -60.0f : 60.0f), 0.0f,
                                           static_cast<float>(Tank.Width - AquaFrameOf(Creature).Width));
            Creature.GoalY    = Creature.Y;
            Creature.GoalLeft = 2.0f;
            if (Kind.Way == AquaWay::Guards) {
                Creature.GoalX = static_cast<float>(Tank.Scene.AnemoneX - 3);
                Creature.GoalY = static_cast<float>(Tank.Scene.AnemoneY - 6);
            }
            break;
        default: break;
        }
    }
}

void AquaAdd(AquaTank &Tank, float X, float Y, AquaDice &Dice)
{
    const std::vector<AquaKind> &Kinds = AquaKinds();
    std::vector<int>             Swimmers;
    for (int Index = 0; Index < static_cast<int>(Kinds.size()); Index++)
        if (Kinds[static_cast<std::size_t>(Index)].Way == AquaWay::Swims) Swimmers.push_back(Index);
    if (Swimmers.empty()) return;

    int          Kind = Swimmers[static_cast<std::size_t>(Dice.Count(0, static_cast<int>(Swimmers.size()) - 1))];
    AquaCreature Born = AquaHatch(Tank, Kind, X, Dice);
    Born.Y     = Y;
    Born.GoalY = Y;
    KeepInWater(Tank, Born);
    Tank.Creatures.push_back(Born);
}
