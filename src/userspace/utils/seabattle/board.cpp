/* board.cpp — placing ships, laying a fleet at random, and taking a shot. */

#include "board.h"

#include <algorithm>

namespace seabattle {

namespace {

constexpr int FillAttempts = 64;

struct Spot {
    int  Row, Col;
    bool Vertical;
};

}  // namespace

Board::Board(int Size) : Wide(Size), Owner(static_cast<std::size_t>(Size) * Size, -1), Marks(static_cast<std::size_t>(Size) * Size, Mark::None)
{
}

bool Board::NextToShip(int Row, int Col) const
{
    for (int DownBy = -1; DownBy <= 1; DownBy++)
        for (int AcrossBy = -1; AcrossBy <= 1; AcrossBy++)
            if (ShipAt(Row + DownBy, Col + AcrossBy) >= 0) return true;
    return false;
}

bool Board::CanPlace(int Row, int Col, int Length, bool Vertical, bool Touching) const
{
    for (int At = 0; At < Length; At++) {
        const int CellRow = Vertical ? Row + At : Row;
        const int CellCol = Vertical ? Col : Col + At;
        if (!Inside(CellRow, CellCol)) return false;
        if (Touching ? ShipAt(CellRow, CellCol) >= 0 : NextToShip(CellRow, CellCol)) return false;
    }
    return true;
}

void Board::Place(int Row, int Col, int Length, bool Vertical)
{
    Fleet.push_back(Ship{Row, Col, Length, Vertical});
    const Ship &Laid = Fleet.back();
    for (int At = 0; At < Length; At++) Owner[Index(Laid.CellRow(At), Laid.CellCol(At))] = static_cast<int>(Fleet.size()) - 1;
}

bool Board::UnplaceLast()
{
    if (Fleet.empty()) return false;
    const Ship &Last = Fleet.back();
    for (int At = 0; At < Last.Length; At++) Owner[Index(Last.CellRow(At), Last.CellCol(At))] = -1;
    Fleet.pop_back();
    return true;
}

void Board::ClearShips()
{
    while (UnplaceLast()) {
    }
}

bool Board::Fill(Luck &Chance, std::span<const int> Lengths, bool Touching)
{
    const std::size_t Before = Fleet.size();
    std::vector<Spot> Open;
    for (int Attempt = 0; Attempt < FillAttempts; Attempt++) {
        bool Stuck = false;
        for (const int Length : Lengths) {
            Open.clear();
            for (int Row = 0; Row < Wide; Row++) {
                for (int Col = 0; Col < Wide; Col++) {
                    if (CanPlace(Row, Col, Length, false, Touching)) Open.push_back({Row, Col, false});
                    if (Length > 1 && CanPlace(Row, Col, Length, true, Touching)) Open.push_back({Row, Col, true});
                }
            }
            if (Open.empty()) {
                Stuck = true;
                break;
            }
            const Spot &Chosen = Open[Chance.Below(static_cast<std::uint32_t>(Open.size()))];
            Place(Chosen.Row, Chosen.Col, Length, Chosen.Vertical);
        }
        if (!Stuck) return true;
        while (Fleet.size() > Before) UnplaceLast();
    }
    return false;
}

void Board::Sink(const Ship &Which, bool Touching)
{
    for (int At = 0; At < Which.Length; At++) Marks[Index(Which.CellRow(At), Which.CellCol(At))] = Mark::Wreck;
    if (Touching) return;
    for (int At = 0; At < Which.Length; At++) {
        for (int DownBy = -1; DownBy <= 1; DownBy++) {
            for (int AcrossBy = -1; AcrossBy <= 1; AcrossBy++) {
                const int Row = Which.CellRow(At) + DownBy, Col = Which.CellCol(At) + AcrossBy;
                if (Inside(Row, Col) && Marks[Index(Row, Col)] == Mark::None) Marks[Index(Row, Col)] = Mark::Halo;
            }
        }
    }
}

ShotResult Board::Fire(int Row, int Col, bool Touching)
{
    const int Which = ShipAt(Row, Col);
    if (Which < 0) {
        Marks[Index(Row, Col)] = Mark::Miss;
        return {};
    }
    Marks[Index(Row, Col)] = Mark::Hit;
    Ship &Struck = Fleet[static_cast<std::size_t>(Which)];
    Struck.Hits++;
    if (!Struck.Sunk()) return {Outcome::Hit, Which};
    Sink(Struck, Touching);
    return {Outcome::Sunk, Which};
}

int Board::Afloat() const
{
    return static_cast<int>(std::count_if(Fleet.begin(), Fleet.end(), [](const Ship &One) { return !One.Sunk(); }));
}

}  // namespace seabattle
