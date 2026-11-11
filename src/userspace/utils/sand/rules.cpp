/* rules.cpp — how cells move: powders fall and pile, liquids fall and run
 * sideways, gases rise and drift. Heavier displaces lighter, so sand sinks
 * through water and oil floats on it, and steam bubbles up through both. */

#include <algorithm>

#include "world.h"

namespace sand {

void World::Step()
{
    Phase ^= 1;
    const int First = Fall > 0 ? Height - 1 : 0;
    const int Last  = Fall > 0 ? -1 : Height;
    const int Next  = -Fall;
    for (int Y = First; Y != Last; Y += Next) {
        const bool Forward = Luck.OneIn(2);
        for (int Column = 0; Column < Width; Column++) Update(Forward ? Column : Width - 1 - Column, Y);
    }
}

void World::Update(int X, int Y)
{
    Cell &Self = Edit(X, Y);
    if (Self.Type == Kind::Air || Self.Moved == Phase) return;
    Self.Moved = Phase;
    const Kind Type = Self.Type;
    switch (TraitOf(Type).Moves) {
    case Habit::Powder: Settle(X, Y); break;
    case Habit::Liquid: Run(X, Y); break;
    case Habit::Gas:    Drift(X, Y); break;
    case Habit::Flame:  Burn(X, Y); break;
    case Habit::Solid:
        if (Type == Kind::Plant) Grow(X, Y);
        break;
    case Habit::Empty:  break;
    }
}

// Whether `Mover` may take the place of what is at X, Y: only air, a liquid
// or a gas gives way, and only to what is heavier — or, for what rises, lighter.
bool World::Yields(Kind Mover, int X, int Y) const
{
    if (!Inside(X, Y)) return false;
    const Trait &Target = TraitOf(At(X, Y).Type);
    if (Target.Moves != Habit::Empty && Target.Moves != Habit::Liquid && Target.Moves != Habit::Gas) return false;
    const int Own = TraitOf(Mover).Density;
    return Own > 0 ? Target.Density < Own : Target.Density > Own;
}

void World::Swap(int FromX, int FromY, int ToX, int ToY)
{
    std::swap(Edit(FromX, FromY), Edit(ToX, ToY));
    Edit(FromX, FromY).Moved = Phase;
    Edit(ToX, ToY).Moved     = Phase;
}

// One cell along the way it falls or rises, or slipping down either side of
// it. Pushing through anything but air costs a toss of the coin.
bool World::Slide(int X, int Y)
{
    const Kind Type   = At(X, Y).Type;
    const int  Toward = TraitOf(Type).Density > 0 ? Fall : -Fall;
    const int  Side   = Luck.OneIn(2) ? 1 : -1;
    const int  Ways[3] = {0, Side, -Side};
    for (const int Way : Ways) {
        const int Nx = X + Way, Ny = Y + Toward;
        if (!Yields(Type, Nx, Ny)) continue;
        if (At(Nx, Ny).Type != Kind::Air && Luck.OneIn(2)) continue;
        Swap(X, Y, Nx, Ny);
        return true;
    }
    return false;
}

// Sideways through free air, as far as `Reach` cells, to the farthest it gets.
bool World::Spread(int X, int Y, int Reach)
{
    const int Side = Luck.OneIn(2) ? 1 : -1;
    for (const int Way : {Side, -Side}) {
        int Landed = 0;
        while (Landed < Reach && Inside(X + Way * (Landed + 1), Y) &&
               At(X + Way * (Landed + 1), Y).Type == Kind::Air)
            Landed++;
        if (Landed == 0) continue;
        Swap(X, Y, X + Way * Landed, Y);
        return true;
    }
    return false;
}

void World::Settle(int X, int Y)
{
    if (!Slide(X, Y) && At(X, Y).Type == Kind::Seed) Sprout(X, Y);
}

void World::Run(int X, int Y)
{
    const Kind Type = At(X, Y).Type;
    if (Type == Kind::Lava) {
        Scorch(X, Y);
        if (At(X, Y).Type != Kind::Lava || !Luck.OneIn(3)) return;
    }
    if (Slide(X, Y)) return;
    const int Reach = Type == Kind::Lava ? 1 : Type == Kind::Oil ? std::max(1, Flow / 2) : Flow;
    Spread(X, Y, Reach);
}

// Smoke and steam thin out as they rise; steam that has cooled falls as water.
void World::Drift(int X, int Y)
{
    Cell &Self = Edit(X, Y);
    if (--Self.Life == 0) {
        Make(Self, Self.Type == Kind::Steam ? Kind::Water : Kind::Air);
        return;
    }
    if (!Luck.OneIn(4) && Slide(X, Y)) return;
    Spread(X, Y, 1);
}

}  // namespace sand
