/* world.h — the grid of cells and the rules that move them. A step visits
 * every cell once, bottom first, each row in a direction picked at random so
 * nothing drifts one way; a cell that has moved is not moved again in the
 * same step. Rules: rules.cpp (movement), chemistry.cpp (fire, lava, growth),
 * world.cpp (the grid, the brush, the weather). */
#ifndef SAND_WORLD_H
#define SAND_WORLD_H

#include <cstdint>
#include <vector>

#include "materials.h"

namespace sand {

struct Cell {
    Kind          Type  = Kind::Air;
    Kind          Was   = Kind::Air;   // what a burning cell was before it caught
    std::uint8_t  Grain = 0;           // fixed at birth: the cell's own shade
    std::uint8_t  Life  = 0;           // how long a flame, smoke or steam has left
    std::uint8_t  Moved = 0;           // the phase of the step that last moved it
};

// A flame with at least this much life is a burning thing that stays where it is.
inline constexpr int FuelFloor = 64;

class Dice {
public:
    explicit Dice(std::uint64_t Seed) : State(Seed | 1) {}

    std::uint32_t Below(std::uint32_t Limit)
    {
        State ^= State >> 12;
        State ^= State << 25;
        State ^= State >> 27;
        return static_cast<std::uint32_t>(((State * 0x2545F4914F6CDD1Dull) >> 32) % Limit);
    }
    bool OneIn(std::uint32_t Odds) { return Below(Odds) == 0; }

private:
    std::uint64_t State;
};

// Whether the cell Dx, Dy from the middle of a brush `Size` pixels wide is under it.
inline bool InBrush(int Dx, int Dy, int Size)
{
    return 4 * (Dx * Dx + Dy * Dy) <= Size * Size + Size;
}

class World {
public:
    explicit World(std::uint64_t Seed) : Luck(Seed) {}

    void Resize(int Wide, int High);
    void Clear();
    void Step();
    // Pour `What` in a disc of `Size` pixels, into a Percent of the free cells; Air erases,
    // and fire lights what burns.
    void Stamp(int X, int Y, int Size, Kind What, int Percent);
    // A cell of `What` falls from the top in one of every `Rarity` columns.
    void Weather(Kind What, int Rarity);
    void Place(int X, int Y, Kind What);

    bool Inside(int X, int Y) const { return X >= 0 && Y >= 0 && X < Width && Y < Height; }
    const Cell &At(int X, int Y) const { return Cells[static_cast<std::size_t>(Y) * Width + X]; }

    int Width  = 0;
    int Height = 0;
    int Flow   = 3;   // how far water runs sideways in a step
    int Fall   = 1;   // +1: things fall toward the last row, -1: toward the first

private:
    Cell &Edit(int X, int Y) { return Cells[static_cast<std::size_t>(Y) * Width + X]; }
    void  Make(Cell &Target, Kind What);

    void Update(int X, int Y);
    bool Yields(Kind Mover, int X, int Y) const;
    void Swap(int FromX, int FromY, int ToX, int ToY);
    bool Slide(int X, int Y);
    bool Spread(int X, int Y, int Reach);
    void Settle(int X, int Y);
    void Run(int X, int Y);
    void Drift(int X, int Y);

    void Burn(int X, int Y);
    void Feed(int X, int Y, bool Hot);
    void Kindle(Cell &Target, int Heat);
    void Scorch(int X, int Y);
    void Grow(int X, int Y);
    void Sprout(int X, int Y);
    bool TakeWater(int X, int Y);

    std::vector<Cell> Cells;
    std::uint8_t      Phase = 0;
    Dice              Luck;
};

// The four neighbors a cell shares a side with.
inline constexpr int Around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

}  // namespace sand

#endif
