/* table.h — the table the dice are thrown on: where each die of a roll comes
 * to rest, and how it gets there — in from the left edge, bouncing, its face
 * changing less and less often until it lands on the one the roll gave. As
 * many dice as the table holds are drawn as dice, more of them as tiles of
 * the number, and the ones that fit no way are counted and left to the panel. */
#ifndef DICE_TABLE_H
#define DICE_TABLE_H

#include <cstdint>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"
#include "roll.h"
#include "source.h"

namespace dice {

class Table {
public:
    // The dice of Thrown set down on an area of Cells, and sent flying.
    void Throw(const Roll &Thrown, painter::core::Size Cells, Source &From);
    // The table in Area of Out, Elapsed milliseconds after the throw.
    void Paint(painter::tui::Frame &Out, painter::core::Rect Area, std::uint64_t ElapsedMs, const Look &Shown) const;
    // Whether the dice have all come to rest, Elapsed milliseconds after the throw.
    static bool Settled(std::uint64_t ElapsedMs, const Look &Shown);

private:
    struct Piece {
        int                      Sides = 0;
        int                      Face = 0;
        bool                     Counts = true;
        painter::core::Point     Rest;      // in pixels, from the table's corner
        painter::core::Point     Start;
        double                   Delay = 0;   // of the throw, from 0 to 1
        std::uint64_t            Seed = 0;
    };

    void Place(painter::core::Size Area);
    void Aim(Source &From);
    painter::core::Point Where(const Piece &One, double Progress) const;

    std::vector<Piece> Pieces;
    painter::core::Size Cells;
    int                 Side = 0;       // a die's side in pixels; 0: tiles
    int                 Digits = 1;     // of the widest face, for a tile
    std::size_t         Placed = 0;     // the pieces that have a place
};

}  // namespace dice

#endif
