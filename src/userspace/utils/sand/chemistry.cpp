/* chemistry.cpp — what things do to each other: fire catches and spreads,
 * water puts it out, lava boils water, melts sand to glass and lights what
 * burns, and plants and seeds grow on the water they drink. */

#include <algorithm>

#include "world.h"

namespace sand {

namespace {

// How much water a plant holds at most, and what a drink and a growth cost.
constexpr int Thirst      = 200;
constexpr int Drink       = 80;
constexpr int GrowthCost  = 40;
constexpr int SproutWater = 90;

// Where a plant may grow from a cell, mostly up: up three times in seven, up
// either side, or along; the second number is multiplied by the way up.
constexpr int Growth[7][2] = {{0, -1}, {0, -1}, {0, -1}, {-1, -1}, {1, -1}, {-1, 0}, {1, 0}};

}  // namespace

// What catches takes the fire's place and burns for as long as its kind does.
void World::Kindle(Cell &Target, int Heat)
{
    const Trait &Fuel = TraitOf(Target.Type);
    if (Fuel.Ignites == 0 || !Luck.OneIn(static_cast<std::uint32_t>(std::max(1, Fuel.Ignites / Heat)))) return;
    Target.Was   = Target.Type;
    Target.Type  = Kind::Fire;
    Target.Life  = static_cast<std::uint8_t>(FuelFloor + Fuel.Fuel + Luck.Below(24));
    Target.Moved = Phase;
}

// A burning thing sends flames and smoke up from itself; oil sends more, and
// bigger.
void World::Feed(int X, int Y, bool Hot)
{
    const int Ax = X + static_cast<int>(Luck.Below(3)) - 1;
    const int Ay = Y - Fall;
    if (!Inside(Ax, Ay) || At(Ax, Ay).Type != Kind::Air) return;
    if (Luck.OneIn(Hot ? 2 : 4)) {
        Cell &Flame = Edit(Ax, Ay);
        Make(Flame, Kind::Fire);
        if (Hot) Flame.Life += 14;
    } else if (Luck.OneIn(6)) {
        Make(Edit(Ax, Ay), Kind::Smoke);
    }
}

void World::Burn(int X, int Y)
{
    const bool Hot    = At(X, Y).Was == Kind::Oil;
    const bool Burning = At(X, Y).Life >= FuelFloor;
    for (const auto &Side : Around) {
        const int Nx = X + Side[0], Ny = Y + Side[1];
        if (!Inside(Nx, Ny)) continue;
        Cell &Next = Edit(Nx, Ny);
        if (Next.Type == Kind::Water) {
            if (Burning) {
                if (Luck.OneIn(40)) Make(Next, Kind::Steam);
                continue;
            }
            if (Luck.OneIn(6)) Make(Next, Kind::Steam);
            Make(Edit(X, Y), Luck.OneIn(2) ? Kind::Smoke : Kind::Air);
            return;
        }
        Kindle(Next, Hot ? 2 : 1);
    }
    Cell &Self = Edit(X, Y);
    if (Burning) {
        if (Luck.OneIn(2)) Self.Life--;
        if (Self.Life < FuelFloor) Make(Self, Luck.OneIn(2) ? Kind::Smoke : Kind::Air);
        else Feed(X, Y, Hot);
        return;
    }
    const int Spent = 1 + static_cast<int>(Luck.Below(3));
    if (Self.Life <= Spent) {
        Make(Self, Luck.OneIn(3) ? Kind::Smoke : Kind::Air);
        return;
    }
    Self.Life = static_cast<std::uint8_t>(Self.Life - Spent);
    if (Luck.OneIn(4)) return;
    if (!Slide(X, Y)) Spread(X, Y, 1);
}

void World::Scorch(int X, int Y)
{
    for (const auto &Side : Around) {
        const int Nx = X + Side[0], Ny = Y + Side[1];
        if (!Inside(Nx, Ny)) continue;
        Cell &Next = Edit(Nx, Ny);
        if (Next.Type == Kind::Water) {
            Make(Next, Kind::Steam);
            if (Luck.OneIn(4)) {
                Make(Edit(X, Y), Kind::Stone);
                return;
            }
        } else if (Next.Type == Kind::Sand) {
            if (Luck.OneIn(12)) Make(Next, Kind::Glass);
        } else {
            Kindle(Next, 3);
        }
    }
    const int Ay = Y - Fall;
    if (Inside(X, Ay) && At(X, Ay).Type == Kind::Air && Luck.OneIn(120)) Make(Edit(X, Ay), Kind::Fire);
}

// Drinks one of the water cells beside X, Y, if there is one.
bool World::TakeWater(int X, int Y)
{
    const int Start = static_cast<int>(Luck.Below(4));
    for (int Try = 0; Try < 4; Try++) {
        const auto &Side = Around[(Start + Try) & 3];
        const int   Nx = X + Side[0], Ny = Y + Side[1];
        if (!Inside(Nx, Ny) || At(Nx, Ny).Type != Kind::Water) continue;
        Make(Edit(Nx, Ny), Kind::Air);
        return true;
    }
    return false;
}

// A plant drinks what it touches, and spends what it holds on growing a shoot
// where none crowds it: the water climbs with the growth, so a drink carries a few cells.
void World::Grow(int X, int Y)
{
    Cell &Self = Edit(X, Y);
    if (Self.Life < Thirst && Luck.OneIn(10) && TakeWater(X, Y))
        Self.Life = static_cast<std::uint8_t>(std::min(255, Self.Life + Drink));
    if (Self.Life < GrowthCost || !Luck.OneIn(10)) return;
    const auto &Way = Growth[Luck.Below(7)];
    const int   Nx = X + Way[0], Ny = Y + Way[1] * Fall;
    if (!Inside(Nx, Ny) || At(Nx, Ny).Type != Kind::Air) return;
    int Crowd = 0;
    for (const auto &Side : Around)
        if (Inside(Nx + Side[0], Ny + Side[1]) && At(Nx + Side[0], Ny + Side[1]).Type == Kind::Plant) Crowd++;
    if (Crowd > 1) return;
    Cell &Shoot = Edit(Nx, Ny);
    Make(Shoot, Kind::Plant);
    Shoot.Life = static_cast<std::uint8_t>(Self.Life - GrowthCost);
    Self.Life  = 0;
}

// A seed at rest on something firm, beside water, takes root.
void World::Sprout(int X, int Y)
{
    const int Below = Y + Fall;
    if (Inside(X, Below)) {
        const Habit Under = TraitOf(At(X, Below).Type).Moves;
        if (Under != Habit::Solid && Under != Habit::Powder) return;
    }
    if (!Luck.OneIn(10) || !TakeWater(X, Y)) return;
    Cell &Root = Edit(X, Y);
    Make(Root, Kind::Plant);
    Root.Life = SproutWater;
}

}  // namespace sand
