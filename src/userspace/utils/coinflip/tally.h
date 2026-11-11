/* tally.h — what the flips of a sitting came to: each face counted, the run
 * it is in and the longest of each, the calls made and how many came true. */
#ifndef COINFLIP_TALLY_H
#define COINFLIP_TALLY_H

#include <cstdint>
#include <optional>
#include <string>

#include "toss.h"

namespace coinflip {

class Tally {
public:
    // A flip that landed on Landed, called as Called when the person called it.
    void Add(Face Landed, std::optional<Face> Called);

    std::uint64_t Count(Face Which) const { return Which == Face::Heads ? Heads : Tails; }
    std::uint64_t Total() const { return Heads + Tails; }
    std::uint64_t Longest(Face Which) const { return Which == Face::Heads ? LongestHeads : LongestTails; }
    std::uint64_t Calls() const { return CallsWon + CallsLost; }
    std::uint64_t Won() const { return CallsWon; }

    // The face the last flips agreed on, and for how many.
    Face          RunFace() const { return Run; }
    std::uint64_t RunLength() const { return RunCount; }

    // The last flips, oldest first: H and T.
    const std::string &Recent() const { return Last; }

private:
    std::uint64_t Heads        = 0;
    std::uint64_t Tails        = 0;
    std::uint64_t LongestHeads = 0;
    std::uint64_t LongestTails = 0;
    std::uint64_t CallsWon     = 0;
    std::uint64_t CallsLost    = 0;
    Face          Run          = Face::Heads;
    std::uint64_t RunCount     = 0;
    std::string   Last;
};

}  // namespace coinflip

#endif
