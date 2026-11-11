/* card_art.h — playing cards as they are drawn: a white card with rounded
 * corners and a soft shadow, its rank and suit in the font at two corners,
 * pips and court figures described as shapes and drawn at whatever size the
 * glass has room for, and a patterned back.
 *
 * A card at rest stands on whole cells so that its indices are the font's own
 * and sharp. A card in flight (sliding from the shoe, or turning over) is
 * drawn to the pixel and a fraction of one, from shapes alone. */
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
    // Too small for drawn pips: the font's suit marks stand in rows.
    bool Glyphs() const { return Cols < 11; }
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
