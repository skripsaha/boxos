/* ship_art.h — a ship seen from above, drawn in pixels for any size of cell:
 * a hull with a rounded stern and a pointed bow, its deck, and what stands on
 * it, different for each length — a patrol boat's cabin, a destroyer's gun,
 * a cruiser's two turrets, a battleship's three and its funnels, a carrier's
 * runway with its island and planes. */
#ifndef SEABATTLE_SHIP_ART_H
#define SEABATTLE_SHIP_ART_H

#include <cstdint>

#include "canvas.h"

namespace seabattle {

struct ShipDraw {
    int           X = 0, Y = 0;         // the top left pixel of the first cell
    int           Cell = 8;             // pixels to a cell
    int           Length = 1;           // in cells
    bool          Vertical = false;     // the bow down, else to the right
    Color         Steel = Color();
    Color         Water = Color();                // what the sea is, for a wreck's waterline
    std::uint64_t Ms = 0;               // the ship rides the swell
    bool          Wreck = false;        // sunk: dark, torn, half under water
    float         Sink = 0.0f;          // 0..1 while it breaks in two and goes down
    Color         Tint = Color();               // a colour laid over it, as much as TintShare says of 256
    int           TintShare = 0;
    float         Alpha = 1.0f;         // how much of it shows; a ghost has less
    bool          Plain = false;        // a hull and its outline only: for a page of sixteen colours
};

void DrawShip(Canvas &Out, const ShipDraw &Ship);

}  // namespace seabattle

#endif
