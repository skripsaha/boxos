/* look.h — what the person set of the casino (settings casino): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef CASINO_LOOK_H
#define CASINO_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "painter/core/color.h"

namespace casino {

using Color = painter::core::Color;

// The keys a person chooses, in the order of Look::Keys; arrows, Enter, Space,
// Backspace, the letters and Esc are the tables' own and no setting takes them.
enum CasinoKey : std::uint8_t { KeySpin, KeyTable, KeyPay, KeyHelp, KeyCount };

struct Look {
    std::int64_t StartChips;     // the stack a new player is given
    bool         Refill;         // a broke player is given a fresh stack
    std::int64_t RefillChips;
    std::int64_t Step;           // the smallest chip
    std::int32_t Speed;          // percent: how fast the wheel and the reels go
    Color        Page;           // behind everything
    Color        Felt;           // the table's cloth
    Color        Wheel;          // the wheel's wood
    Color        Cabinet;        // the slot machine's body
    Color        Lights;         // the bulbs round the table and the cabinet
    Color        Red;            // red pockets and cells
    Color        Black;          // black pockets and cells
    Color        Gold;           // trim, titles, the jackpot
    Color        Text;
    Color        Quiet;
    Color        Won;
    Color        Lost;
    KeyChord     Keys[KeyCount];
};

Look ReadLook();

}  // namespace casino

#endif
