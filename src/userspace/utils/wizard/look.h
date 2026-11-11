/* look.h — what the person set of the wizard (settings wizard): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef WIZARD_LOOK_H
#define WIZARD_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "painter/core/color.h"

namespace wizard {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; the digits, backspace
// and esc are fixed, as typing is.
enum WizardKey : std::uint8_t { KeySpell, KeyDrink, KeyEmpty, KeyLift, KeyHelp, KeyCount };

struct Look {
    std::uint32_t DurationSeconds;   // how long a spell or a potion lasts, at most
    std::uint32_t StrengthPercent;   // how strongly it is felt, at most
    std::uint32_t Allowed;           // the DISPLAY_EFFECT_* kinds that may be cast
    Color         Room;              // the wall,
    Color         Floor;             //   the floor,
    Color         Glow;              //   what magic glows in
    Color         Paper;             // the wizard's speech
    Color         Ink;
    Color         Bars;              // the lines at the top and the bottom
    Color         Title;
    Color         Text;
    Color         Quiet;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace wizard

#endif
