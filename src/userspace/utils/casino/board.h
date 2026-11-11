/* board.h — the betting board as the roulette table lays it out: the zero on
 * the left, the numbers in three rows of twelve, a column bet at the end of
 * each row, the dozens under the numbers and the even-money bets under them.
 * The grid is 14 wide and 5 tall; a cell that spans several is one place to
 * put chips. The cursor walks the places. */
#ifndef CASINO_BOARD_H
#define CASINO_BOARD_H

#include "bets.h"

namespace casino {

inline constexpr int BoardWide = 14;
inline constexpr int BoardHigh = 5;

// A place on the board and the grid cells it spans, both ends included.
struct Place {
    Spot On;
    int  Left  = 0;
    int  Right = 0;
    int  Top   = 0;
    int  Bottom = 0;
};

// The number in the grid cell (Across, Down) of the numbers' block: 1 to 36.
int NumberIn(int Across, int Down);

// The place that holds the grid cell (Across, Down), or null.
const Place *PlaceAt(int Across, int Down);
const Place *PlaceOf(const Spot &On);

class Cursor {
public:
    // One step; a place that spans several cells is crossed in one step.
    void Go(int Across, int Down);
    const Place &Here() const;
    int  Across() const { return Col; }
    int  Down() const { return Row; }

private:
    int Col = 1;
    int Row = 1;
};

}  // namespace casino

#endif
