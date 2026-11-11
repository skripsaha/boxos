/* card_art.h — playing cards as they are drawn: a white card with rounded
 * corners and a thin shadow, its rank and suit in the font at two corners and
 * its pips the font's own suit marks laid on the cells in the classic
 * arrangement; a court card is a framed portrait of a crowned head and
 * shoulders, turned round for its lower half; the back is a lattice with an
 * emblem.
 *
 * A card at rest stands on whole cells. A card in flight (sliding from the
 * shoe, or turning over) is drawn to the pixel and a fraction of one, as a
 * blank face or a back. */
#ifndef CASINO_CARD_ART_H
#define CASINO_CARD_ART_H

#include <cstdint>

#include "cards.h"
#include "look.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace casino {

// A card's size in cells; it is as many pixels across and twice the rows down.
struct CardSize {
    std::int32_t Cols = 9;
    std::int32_t Rows = 7;

    std::int32_t Wide() const { return Cols; }
    std::int32_t High() const { return Rows * 2; }
};

// The largest card the glass has the rows for.
CardSize CardSizeFor(std::int32_t GlassRows);

// A card at rest with its top left on cell At. Face null is its back.
void PaintCard(painter::tui::Frame &Out, const Look &Shown, const Card *Face, painter::core::Point At, CardSize Size);

// A card in flight, its top left Across and Down pixels from the glass's, as
// wide as Squeeze (0 to 1) of itself.
void PaintMoving(painter::tui::Frame &Out, const Look &Shown, const Card *Face, float Across, float Down, CardSize Size,
                 float Squeeze);

}  // namespace casino

#endif
