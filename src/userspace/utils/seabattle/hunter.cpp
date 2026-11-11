/* hunter.cpp — the computer's choice of a cell (hunter.h). */

#include "hunter.h"

#include <algorithm>
#include <vector>

namespace seabattle {

namespace {

constexpr long WoundedWeight = 24;

// What a way for a ship to lie through these cells is worth: 0 when a miss, a sunk ship
// or a halo is in it, else 1, and many times more for each wound it runs through.
long WayWeight(const Board &Waters, int Row, int Col, int Length, bool Vertical)
{
    long Weight = 1;
    for (int At = 0; At < Length; At++) {
        const Mark Known = Waters.MarkAt(Vertical ? Row + At : Row, Vertical ? Col : Col + At);
        if (Known != Mark::None && Known != Mark::Hit) return 0;
        if (Known == Mark::Hit) Weight *= WoundedWeight;
    }
    return Weight;
}

}  // namespace

Aim Hunt(const Board &Waters, Luck &Chance)
{
    const int Size = Waters.Size();
    std::vector<long> Worth(static_cast<std::size_t>(Size) * Size, 0);
    for (const Ship &Afloat : Waters.Ships()) {
        if (Afloat.Sunk()) continue;
        for (int Vertical = 0; Vertical <= 1; Vertical++) {
            if (Vertical == 1 && Afloat.Length == 1) break;
            const int Down = Vertical == 1 ? Afloat.Length : 1, Across = Vertical == 1 ? 1 : Afloat.Length;
            for (int Row = 0; Row + Down <= Size; Row++) {
                for (int Col = 0; Col + Across <= Size; Col++) {
                    const long Weight = WayWeight(Waters, Row, Col, Afloat.Length, Vertical == 1);
                    if (Weight == 0) continue;
                    for (int At = 0; At < Afloat.Length; At++) {
                        const int CellRow = Vertical == 1 ? Row + At : Row, CellCol = Vertical == 1 ? Col : Col + At;
                        if (Waters.MarkAt(CellRow, CellCol) == Mark::None) Worth[static_cast<std::size_t>(CellRow) * Size + CellCol] += Weight;
                    }
                }
            }
        }
    }
    long Best = 0;
    for (int Row = 0; Row < Size; Row++)
        for (int Col = 0; Col < Size; Col++)
            if (Waters.MarkAt(Row, Col) == Mark::None) Best = std::max(Best, Worth[static_cast<std::size_t>(Row) * Size + Col]);
    std::vector<Aim> Tied;
    for (int Row = 0; Row < Size; Row++)
        for (int Col = 0; Col < Size; Col++)
            if (Waters.MarkAt(Row, Col) == Mark::None && Worth[static_cast<std::size_t>(Row) * Size + Col] == Best) Tied.push_back({Row, Col});
    return Tied[Chance.Below(static_cast<std::uint32_t>(Tied.size()))];
}

}  // namespace seabattle
