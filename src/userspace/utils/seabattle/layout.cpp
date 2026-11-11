/* layout.cpp — the places of the waters on the glass (layout.h). */

#include "layout.h"

#include <algorithm>

namespace seabattle {

namespace {

constexpr int TopRows = 4;           // the bar, the name, the letters and the frame's top
constexpr int BlockMargins = 4;      // the numbers and the frame's two sides, besides the grid
constexpr int LeastGap = 3;
constexpr int MostCell = 14;

// The text rows under a grid that the battle keeps: the frame's foot, the fleets' rows, the
// figures, the line of words and the keys.
constexpr int BattleFoot = 1 + 2 + 1 + 1 + 1;

// The victory: rows above the boards for the winner's name, and under them for the frame's foot,
// seven lines of figures, the choices and the keys.
constexpr int RevealBanner = 2;
constexpr int RevealFoot = 1 + 8 + 2 + 1;

int Fitting(int Rows, int Foot, int Size) { return std::clamp((Rows - TopRows - Foot) * 2 / Size, 2, MostCell); }

}  // namespace

Arena ArenaFor(painter::core::Size Glass, int Size)
{
    const auto Smaller = [](int Cell) { return std::max(2, Cell * 3 / 4); };
    int        Large = Fitting(Glass.Height, BattleFoot, Size);
    while (Large > 2 && Size * (Large + Smaller(Large)) + 2 * BlockMargins + LeastGap > Glass.Width) Large--;
    const int Small = Smaller(Large);
    const int Used = Size * (Large + Small) + 2 * BlockMargins;
    const int Gap = std::clamp((Glass.Width - Used) / 2, LeastGap, 14);
    const int Left = std::max(0, (Glass.Width - Used - Gap) / 2);
    const int LargeBlock = Size * Large + BlockMargins;
    return Arena{GridView{Left + 3, TopRows * 2, Large, Size}, GridView{Left + LargeBlock + Gap + 3, TopRows * 2, Small, Size}};
}

GridView PlacementFor(painter::core::Size Glass, int Size)
{
    return GridView{6, TopRows * 2, Fitting(Glass.Height, 1 + 2 + 1, Size), Size};
}

Arena RevealFor(painter::core::Size Glass, int Size)
{
    const int Cell = std::clamp(std::min((Glass.Width - 2 * BlockMargins - 6) / (2 * Size), (Glass.Height - TopRows - RevealBanner - RevealFoot) * 2 / Size), 2, MostCell);
    const int Block = Size * Cell + BlockMargins;
    const int Left = std::max(0, (Glass.Width - 2 * Block - 6) / 2);
    const int Top = (TopRows + RevealBanner) * 2;
    return Arena{GridView{Left + 3, Top, Cell, Size}, GridView{Left + Block + 6 + 3, Top, Cell, Size}};
}

}  // namespace seabattle
