/* aquarium_scene.cpp — the tank and what stands still in it, laid once.
 *
 * The tank is three windows wide and two deep, so there is somewhere to go
 * with the arrows; what lives in it is counted by its size, so a bigger screen
 * has more water and more to swim in it, never a crowd in a small one. */

#include <algorithm>

#include "aquarium.h"

namespace {

constexpr int AirRows = 4;     /* above the water, under the lid */

/* A tank of 240 x 100 pixels holds the counts in AquaKinds; others by area. */
constexpr float BaseArea = 240.0f * 100.0f;

const Color KelpInks[] = {COLOR_RGB(0x2E, 0x8B, 0x3A), COLOR_RGB(0x3A, 0xA0, 0x4A),
                          COLOR_RGB(0x1F, 0x70, 0x2E), COLOR_RGB(0x6A, 0x8A, 0x2A)};
const Color CoralInks[] = {COLOR_RGB(0xFF, 0x6A, 0x50), COLOR_RGB(0xC0, 0x40, 0xA0),
                           COLOR_RGB(0xFF, 0x9A, 0x30), COLOR_RGB(0xE8, 0xD0, 0x40)};
/* Light enough that the depth's shade leaves them dark grey, not black, on a
 * sixteen-colour console. */
const Color RockInks[]  = {COLOR_RGB(0x8A, 0x8A, 0x92), COLOR_RGB(0x7E, 0x82, 0x8C),
                           COLOR_RGB(0x96, 0x8E, 0x82)};
const Color ShellInks[] = {COLOR_RGB(0xF0, 0xE0, 0xD0), COLOR_RGB(0xF0, 0xB0, 0xB0),
                           COLOR_RGB(0xE8, 0xE8, 0xF0)};

template <std::size_t N>
Color Pick(const Color (&Inks)[N], AquaDice &Dice)
{
    return Inks[static_cast<std::size_t>(Dice.Count(0, static_cast<int>(N) - 1))];
}

void LayFloor(AquaTank &Tank, AquaDice &Dice)
{
    AquaScene &Scene = Tank.Scene;
    int   Sand   = AquaSandDepth(Tank);
    float PhaseA = Dice.Unit(), PhaseB = Dice.Unit();
    Scene.Floor.resize(static_cast<std::size_t>(Tank.Width));
    for (int X = 0; X < Tank.Width; X++) {
        float Dune = 2.5f * AquaWave(X / 96.0f + PhaseA) + 1.2f * AquaWave(X / 31.0f + PhaseB);
        Scene.Floor[static_cast<std::size_t>(X)] = Tank.Height - Sand - static_cast<int>(Dune);
    }
}

/* The far hills move at half the speed of the tank, so they need half its
 * width and a window more. */
void LayFar(AquaTank &Tank, int ViewWidth, AquaDice &Dice)
{
    int   Width  = Tank.Width / 2 + ViewWidth + 1;
    float Phase  = Dice.Unit();
    int   Height = (Tank.Height - Tank.Surface) / 4;
    Tank.Scene.Far.resize(static_cast<std::size_t>(Width));
    for (int X = 0; X < Width; X++) {
        float Hill = 0.6f + 0.3f * AquaWave(X / 70.0f + Phase) + 0.15f * AquaWave(X / 23.0f + 2 * Phase);
        Tank.Scene.Far[static_cast<std::size_t>(X)] = static_cast<int>(Height * Hill);
    }
}

void LayStalks(AquaTank &Tank, AquaDice &Dice)
{
    int Water = Tank.Height - Tank.Surface;
    for (int Kelp = Tank.Width / 26; Kelp > 0; Kelp--)
        Tank.Scene.Stalks.push_back({Dice.Count(2, Tank.Width - 3), static_cast<int>(Water * Dice.Between(0.25f, 0.65f)),
                                     Dice.Unit(), Pick(KelpInks, Dice), true});
    for (int Grass = Tank.Width / 6; Grass > 0; Grass--)
        Tank.Scene.Stalks.push_back({Dice.Count(1, Tank.Width - 2), Dice.Count(3, 7), Dice.Unit(),
                                     Pick(KelpInks, Dice), false});
}

/* A branching coral grown upward from the middle of its foot. */
AquaCoral GrowCoral(int X, int Floor, AquaDice &Dice)
{
    AquaCoral Coral;
    Coral.Width  = Dice.Count(7, 11);
    Coral.Height = Dice.Count(6, 10);
    Coral.X      = X - Coral.Width / 2;
    Coral.Y      = Floor - Coral.Height + 1;
    Coral.Ink    = Pick(CoralInks, Dice);
    Coral.Tip    = color_mix(Coral.Ink, COLOR_WHITE, 110);
    Coral.Pixels.assign(static_cast<std::size_t>(Coral.Width * Coral.Height), 0);

    struct Branch { int X, Y; };
    std::vector<Branch> Growing{{Coral.Width / 2, Coral.Height - 1}};
    while (!Growing.empty()) {
        Branch Tip = Growing.back();
        Growing.pop_back();
        while (Tip.Y >= 0) {
            Coral.Pixels[static_cast<std::size_t>(Tip.Y * Coral.Width + Tip.X)] = 1;
            if (Tip.Y == 0 || Dice.Chance(0.12f)) break;
            if (Growing.size() < 6 && Dice.Chance(0.3f)) Growing.push_back({Tip.X, Tip.Y - 1});
            Tip.Y--;
            Tip.X = std::clamp(Tip.X + Dice.Count(-1, 1), 0, Coral.Width - 1);
        }
        Coral.Pixels[static_cast<std::size_t>(std::max(Tip.Y, 0) * Coral.Width + Tip.X)] = 2;
    }
    return Coral;
}

void LayStones(AquaTank &Tank, AquaDice &Dice)
{
    AquaScene &Scene = Tank.Scene;
    for (int Rock = std::max(2, Tank.Width / 50); Rock > 0; Rock--) {
        int Width  = Dice.Count(10, 22);
        int Height = Dice.Count(5, 10);
        int X      = Dice.Count(0, Tank.Width - Width);
        int Floor  = AquaFloor(Tank, X + Width / 2);
        Scene.Rocks.push_back({X, Floor - Height + 2, Width, Height, false, Pick(RockInks, Dice)});
    }
    Scene.Rocks[static_cast<std::size_t>(Dice.Count(0, static_cast<int>(Scene.Rocks.size()) - 1))].Cave = true;

    for (int Coral = std::max(2, Tank.Width / 40); Coral > 0; Coral--) {
        int X = Dice.Count(6, Tank.Width - 7);
        Scene.Corals.push_back(GrowCoral(X, AquaFloor(Tank, X), Dice));
    }
    for (int Shell = Tank.Width / 25; Shell > 0; Shell--) {
        int X = Dice.Count(1, Tank.Width - 2);
        Scene.Shells.push_back({X, AquaFloor(Tank, X) + Dice.Count(0, 2), Pick(ShellInks, Dice)});
    }
}

/* The chest, the anemone and the air stone stand apart, a third of the tank
 * each, so nothing is laid over them. */
void LayLandmarks(AquaTank &Tank, AquaDice &Dice)
{
    AquaScene &Scene = Tank.Scene;
    int Third = Tank.Width / 3;
    Scene.Chest.X    = Dice.Count(Third / 4, Third - 12);
    Scene.Chest.Y    = AquaFloor(Tank, Scene.Chest.X + 4) - 5;
    Scene.Chest.Wait = Dice.Between(8.0f, 20.0f);
    Scene.AnemoneX   = Dice.Count(Third + Third / 4, 2 * Third - Third / 4);
    Scene.AnemoneY   = AquaFloor(Tank, Scene.AnemoneX);
    Scene.BubblerX   = Dice.Count(2 * Third + Third / 4, Tank.Width - Third / 4);
    Scene.BubblerY   = AquaFloor(Tank, Scene.BubblerX);
}

/* Where in the water a band of depth is, for a creature `Height` tall at X. */
float BandY(const AquaTank &Tank, float Band, int Height, int X)
{
    float Top    = static_cast<float>(Tank.Surface + 1);
    float Bottom = static_cast<float>(AquaFloor(Tank, X) - Height - 1);
    return Top + Band * (Bottom - Top);
}

AquaCreature Hatch(const AquaTank &Tank, int KindIndex, float X, AquaDice &Dice)
{
    const AquaKind   &Kind   = AquaKinds()[static_cast<std::size_t>(KindIndex)];
    const AquaSprite &Sprite = Kind.Frames[0];
    AquaCreature      Born;
    Born.Kind   = KindIndex;
    Born.X      = std::clamp(X, 0.0f, static_cast<float>(Tank.Width - Sprite.Width));
    Born.Y      = BandY(Tank, Dice.Between(Kind.Top, Kind.Bottom), Sprite.Height, static_cast<int>(Born.X));
    Born.GoalX  = Born.X;
    Born.GoalY  = Born.Y;
    Born.Stroke = Dice.Unit();
    Born.Phase  = Dice.Unit();
    Born.Hunger = Dice.Between(0.0f, 0.5f);
    Born.Left   = Dice.Chance(0.5f);
    bool OnSand = Kind.Way == AquaWay::Walks || Kind.Way == AquaWay::Creeps || Kind.Way == AquaWay::Rests;
    if (OnSand) Born.Y = static_cast<float>(AquaFloor(Tank, static_cast<int>(Born.X) + Sprite.Width / 2) - Sprite.Height + 1);
    return Born;
}

void Populate(AquaTank &Tank, int Fish, AquaDice &Dice)
{
    const std::vector<AquaKind> &Kinds = AquaKinds();
    float Scale = static_cast<float>(Tank.Width) * static_cast<float>(Tank.Height) / BaseArea;

    /* With a number asked for, every kind that swims is scaled so the swimmers
     * come to about that many; the rest of the tank stays as it is. */
    float Swimmers = 0;
    for (const AquaKind &Kind : Kinds) {
        float Each = Kind.Way == AquaWay::Schools ? 7.0f : 1.0f;
        if (Kind.Way == AquaWay::Swims || Kind.Way == AquaWay::Schools || Kind.Way == AquaWay::Guards)
            Swimmers += Each * (Kind.Fewest + Kind.Most) / 2.0f * Scale;
    }
    float Swim = Fish > 0 && Swimmers > 0 ? Fish / Swimmers : 1.0f;

    for (int Index = 0; Index < static_cast<int>(Kinds.size()); Index++) {
        const AquaKind &Kind = Kinds[static_cast<std::size_t>(Index)];
        bool  Swimming = Kind.Way == AquaWay::Swims || Kind.Way == AquaWay::Schools;
        float Times    = Kind.Way == AquaWay::Guards ? 1.0f : Scale * (Swimming ? Swim : 1.0f);
        int   Count    = static_cast<int>(static_cast<float>(Dice.Count(Kind.Fewest, Kind.Most)) * Times + 0.5f);
        if (Kind.Way == AquaWay::Guards) Count = Kind.Fewest;

        for (int Made = 0; Made < Count; Made++) {
            float X = Kind.Way == AquaWay::Guards ? static_cast<float>(Tank.Scene.AnemoneX + Dice.Count(-8, 8))
                                                  : Dice.Between(0.0f, static_cast<float>(Tank.Width));
            AquaCreature Born = Hatch(Tank, Index, X, Dice);
            if (Kind.Way != AquaWay::Schools) {
                Tank.Creatures.push_back(Born);
                continue;
            }
            int Leader = static_cast<int>(Tank.Creatures.size());
            Tank.Creatures.push_back(Born);
            for (int Follower = Dice.Count(4, 8); Follower > 0; Follower--) {
                AquaCreature Mate = Hatch(Tank, Index, Born.X + Dice.Between(-12.0f, 12.0f), Dice);
                Mate.Leader  = Leader;
                Mate.OffsetX = Dice.Between(-14.0f, 4.0f);
                Mate.OffsetY = Dice.Between(-5.0f, 5.0f);
                Mate.Y       = Born.Y + Mate.OffsetY;
                Tank.Creatures.push_back(Mate);
            }
        }
    }
}

}

int AquaSandDepth(const AquaTank &Tank)
{
    return std::max(6, Tank.Height / 12);
}

int AquaFloor(const AquaTank &Tank, int X)
{
    X = std::clamp(X, 0, Tank.Width - 1);
    return Tank.Scene.Floor[static_cast<std::size_t>(X)];
}

void AquaMakeScene(AquaTank &Tank, AquaDice &Dice)
{
    LayFloor(Tank, Dice);
    LayStalks(Tank, Dice);
    LayStones(Tank, Dice);
    LayLandmarks(Tank, Dice);
}

AquaTank AquaMakeTank(int ViewWidth, int ViewHeight, int Fish, AquaDice &Dice)
{
    AquaTank Tank;
    Tank.Width       = ViewWidth * 3;
    Tank.Height      = ViewHeight * 2;
    Tank.Surface     = AirRows;
    Tank.VisitorWait = Dice.Between(60.0f, 180.0f);
    AquaMakeScene(Tank, Dice);
    LayFar(Tank, ViewWidth, Dice);
    Populate(Tank, Fish, Dice);
    return Tank;
}

AquaCreature AquaHatch(const AquaTank &Tank, int Kind, float X, AquaDice &Dice)
{
    return Hatch(Tank, Kind, X, Dice);
}

float AquaBandY(const AquaTank &Tank, float Band, int Height, int X)
{
    return BandY(Tank, Band, Height, X);
}
