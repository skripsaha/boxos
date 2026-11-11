/* look.h — what the person set of the sandbox (settings sand): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef SAND_LOOK_H
#define SAND_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "materials.h"
#include "painter/core/color.h"

namespace sand {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, the number keys, Enter, Space, Esc and the letters, which no
// setting takes.
enum SandKey : std::uint8_t { KeyHelp, KeyFaster, KeySlower, KeyGravity, KeyCount };

struct Look {
    std::uint32_t Speed;                // steps a second
    int           Flow;                 // how far water runs sideways in a step
    bool          GravityUp;
    int           Brush;                // pixels across
    int           Variety;              // how much grains differ in color, in percent
    Color         Colors[KindCount];    // by kind; the air's is the page's
    Color         Cursor;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace sand

#endif
