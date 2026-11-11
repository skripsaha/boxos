/* board.h — one player's waters: the ships laid on them and every shot that
 * has been fired at them. The rules of placing and of hitting live here and
 * nothing of the picture does. */
#ifndef SEABATTLE_BOARD_H
#define SEABATTLE_BOARD_H

#include <cstdint>
#include <span>
#include <vector>

#include "luck.h"

namespace seabattle {

// What is known of a cell from the shots at it. A wreck is a hit on a ship that has
// sunk; a halo is a cell round a sunk ship, where no ship can lie.
enum class Mark : std::uint8_t { None, Miss, Hit, Wreck, Halo };

enum class Outcome : std::uint8_t { Miss, Hit, Sunk };

struct Ship {
    int  Row, Col;
    int  Length;
    bool Vertical;
    int  Hits = 0;

    bool Sunk() const { return Hits >= Length; }
    int  CellRow(int Index) const { return Vertical ? Row + Index : Row; }
    int  CellCol(int Index) const { return Vertical ? Col : Col + Index; }
};

struct ShotResult {
    Outcome Result = Outcome::Miss;
    int     ShipIndex = -1;     // the ship hit or sunk
};

class Board {
public:
    explicit Board(int Size);

    int  Size() const { return Wide; }
    bool Inside(int Row, int Col) const { return Row >= 0 && Row < Wide && Col >= 0 && Col < Wide; }
    // The ship on a cell, as a place in Ships(), or -1.
    int  ShipAt(int Row, int Col) const { return Inside(Row, Col) ? Owner[Index(Row, Col)] : -1; }
    Mark MarkAt(int Row, int Col) const { return Inside(Row, Col) ? Marks[Index(Row, Col)] : Mark::None; }
    std::span<const Ship> Ships() const { return Fleet; }

    // Whether a ship fits there: in the grid, on free cells, and, unless ships may touch,
    // with no ship in the cells round it.
    bool CanPlace(int Row, int Col, int Length, bool Vertical, bool Touching) const;
    // Whether a cell lies next to a ship, where another may not be laid when ships may not touch.
    bool NextToShip(int Row, int Col) const;
    void Place(int Row, int Col, int Length, bool Vertical);
    bool UnplaceLast();
    void ClearShips();
    // The ships of Lengths laid at random, each on a place chosen evenly among those it may
    // take; false, and the board as it was, when no way is found.
    bool Fill(Luck &Chance, std::span<const int> Lengths, bool Touching);

    // A shot at a cell that has none yet. The cells round a sunk ship are marked as halo
    // unless ships may touch.
    ShotResult Fire(int Row, int Col, bool Touching);
    int        Afloat() const;
    bool       AllSunk() const { return !Fleet.empty() && Afloat() == 0; }

private:
    std::size_t Index(int Row, int Col) const { return static_cast<std::size_t>(Row) * Wide + Col; }
    void        Sink(const Ship &Which, bool Touching);

    int                Wide;
    std::vector<int>   Owner;
    std::vector<Mark>  Marks;
    std::vector<Ship>  Fleet;
};

}  // namespace seabattle

#endif
