/* roll.h — an expression thrown: every die's face, which of them count, each
 * group's sum and the total. */
#ifndef DICE_ROLL_H
#define DICE_ROLL_H

#include <cstdint>
#include <string>
#include <vector>

#include "expression.h"
#include "source.h"

namespace dice {

struct Die {
    int  Value = 0;
    bool Counts = true;
};

struct GroupRoll {
    Term             Of;
    std::vector<Die> Dice;
    std::int64_t     Sum = 0;     // signed: a term taken away is negative
};

struct Roll {
    std::vector<GroupRoll> Groups;
    std::int64_t           Total = 0;
};

// How a face stands among the faces its die has: the highest, the lowest, or between.
enum class Standing : std::uint8_t { Middle, High, Low };

Roll     Throw(const Expression &Of, Source &From);
Standing StandingOf(int Sides, int Value);

// The roll as lines of plain text, for what another command takes.
std::string PlainText(const Roll &Thrown);

}  // namespace dice

#endif
