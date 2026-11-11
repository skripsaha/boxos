/* coin.h — the coin as an object: a disc with a thickness, turning about the
 * line from left to right, seen from the front. Every pixel is the nearest
 * point of the disc along the line of sight — a face, or the rim — lit from
 * above and to the left, so the shine moves as the coin turns. */
#ifndef COINFLIP_COIN_H
#define COINFLIP_COIN_H

#include "painter/tui/frame.h"

#include "look.h"

namespace coinflip {

// Half the thickness of the coin, in radii.
inline constexpr double HalfThickness = 0.1;

// How far the coin reaches above and below its centre when turned by Turn,
// in radii: the whole of it face-on, its thickness edge-on.
double Reach(double Turn);

// The coin of Radius pixels, its centre at (X, Y), turned by Turn (0 shows
// heads, pi tails).
void PaintCoin(painter::tui::Frame &Out, const Look &Shown, double X, double Y, double Radius, double Turn);

}  // namespace coinflip

#endif
