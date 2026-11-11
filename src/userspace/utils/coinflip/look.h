/* look.h — what the person set of the coin (settings coinflip): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef COINFLIP_LOOK_H
#define COINFLIP_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "painter/core/color.h"

namespace coinflip {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; Space and Enter flip,
// H and T call, Esc leaves, and no setting takes them.
enum CoinKey : std::uint8_t { KeyFlip, KeyHeads, KeyTails, KeyHelp, KeyCount };

struct Look {
    Color         Metal;       // what the coin is struck from
    Color         Edge;        // its rim, and the dark of its shading: the metal, darker
    Color         Page;        // behind the coin
    Color         Floor;       // what it falls on
    Color         Text;
    Color         Quiet;       // what is said quietly
    Color         HeadsInk;    // the word HEADS
    Color         TailsInk;    // the word TAILS
    Color         Won;         // a call that came true
    Color         Lost;        // a call that did not
    std::int32_t  Size;        // percent of the room the coin takes
    std::int32_t  Speed;       // percent: how fast a toss goes
    std::int32_t  Quicker;     // percent: how much quicker the flips of a count go
    std::int32_t  Spins;       // turns in the air
    std::int32_t  Bounces;     // how many times it bounces before it lies
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace coinflip

#endif
