/* look.h — what the person set of the scratchpad (settings scratchpad): read
 * whole, and read again when the file is saved or the Use Context changes; and
 * the colors a note is given. */
#ifndef SCRATCHPAD_LOOK_H
#define SCRATCHPAD_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "painter/core/color.h"

namespace scratchpad {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, Home, End, Enter, Backspace, Delete, the page keys, Esc and typing,
// which no setting takes.
enum PadKey : std::uint8_t { KeyColor, KeyTrash, KeyAll, KeyHelp, KeyCount };

// How many papers a palette holds: one setting each.
inline constexpr std::size_t PaletteSize = 8;

// A note's paper and the ink written on it.
struct Sheet {
    Color Paper;
    Color Ink;

    friend constexpr bool operator==(Sheet, Sheet) noexcept = default;
};

struct Look {
    bool         Random;                 // a new note takes a paper from the palette
    Sheet        Fixed;                  // the paper and ink of a new note when it does not
    Color        Palette[PaletteSize];
    std::int32_t Width;                  // the note's size, in cells of the glass
    std::int32_t Height;
    KeyChord     Keys[KeyCount];
};

Look ReadLook();

// The sheet of a new note: the person's fixed one, or a random paper of the
// palette with an ink that reads on it.
Sheet NewSheet(const Look &Shown);

// Another random paper of the palette than Now's, with an ink that reads on it.
Sheet OtherSheet(const Look &Shown, Sheet Now);

}  // namespace scratchpad

#endif
