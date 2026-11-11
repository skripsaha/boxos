/* look.h — what the person set of the dice (settings dice): read whole, and
 * read again when the file is saved or the Use Context changes. */
#ifndef DICE_LOOK_H
#define DICE_LOOK_H

#include <cstdint>
#include <string>

#include "key_chord.h"
#include "painter/core/color.h"

namespace dice {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; the rest are Enter,
// Space, Backspace, Esc and typing, which no setting takes.
enum DiceKey : std::uint8_t { KeyRoll, KeyHelp, KeyCount };

struct Look {
    std::string   Expression;       // rolled when no words are given
    std::uint32_t TumbleMs;         // how long the dice tumble; 0 for none
    std::uint32_t FlipsPerSecond;   // how often a tumbling die shows another face
    std::uint32_t FramesPerSecond;
    Color         Table;
    Color         Body;             // a die
    Color         Ink;              // its pips and its number
    Color         High;             // a highest face
    Color         Low;              // a lowest face
    Color         Bars;             // the lines at the top and the bottom
    Color         Panel;            // behind the result
    Color         Text;
    Color         Quiet;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace dice

#endif
