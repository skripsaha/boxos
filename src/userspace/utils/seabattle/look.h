/* look.h — what the person set of the sea battle (settings seabattle): read
 * whole, and read again when the file is saved or the Use Context changes. */
#ifndef SEABATTLE_LOOK_H
#define SEABATTLE_LOOK_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "key_chord.h"

#include "numbers.h"
#include "rules.h"

namespace seabattle {

// The keys a person chooses, in the order of Look::Keys; the arrows, enter, space, backspace,
// tab and esc are fixed, as typing is.
enum SeaKey : std::uint8_t { KeyRotate, KeyAuto, KeyUndo, KeyClear, KeyHelp, KeyCount };

// How fast the shells fly and the ships sink.
enum class Pace : std::uint8_t { Slow, Normal, Fast, Instant };

struct Look {
    std::string NameOne;
    std::string NameTwo;
    Rules       Start;       // the rules a new game begins with
    Pace        Speed;
    Color       Sea;
    Color       Ship;
    Color       Fire;
    Color       Splash;
    Color       Bars;        // the lines at the top and the bottom, and what is framed
    Color       Title;
    Color       Text;
    Color       Quiet;
    Color       Good;
    Color       Bad;
    KeyChord    Keys[KeyCount];
};

Look ReadLook();

// The look as a text page can show it: it has sixteen colours and tells one by its hue, so the sea,
// the steel and the fire are the page's own blue, grey and red whatever the person chose.
Look Adapted(Look Wanted, bool Text);

// How long a thing that takes Ms at normal pace takes at this one.
std::uint32_t Paced(Pace Speed, std::uint32_t Ms);

}  // namespace seabattle

#endif
