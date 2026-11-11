/* slots.h — the word's slots: each place a big letter over a rule, the letter
 * rising into view when it is found, or a letter of the font's own size for a
 * word too long for the big ones. */
#ifndef HANGMAN_SLOTS_H
#define HANGMAN_SLOTS_H

#include <cstdint>

#include "board.h"
#include "pen.h"

namespace hangman {

// The rows of cells the slots take.
inline constexpr std::int32_t SlotRows = 5;

// The slots centred in Page's area, from the row Row down.
void PaintSlots(const Pen &Page, const Facts &Now, std::int32_t Row);

}  // namespace hangman

#endif
