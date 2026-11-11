/* luck.h — every chance of the sea battle: where a fleet is laid at random,
 * which cell the computer tries among equals. The processor's own entropy
 * (RDSEED, else RDRAND) where it has any; a machine with neither falls back to
 * xoshiro256** seeded through SplitMix64 from the time stamp counter and the
 * clock, and stirred by whatever the players do: the instant of every key is
 * folded in. Ranges are unbiased. */
#ifndef SEABATTLE_LUCK_H
#define SEABATTLE_LUCK_H

#include <cstdint>

#include "box/cpu.h"
#include "box/cxx/cpu.h"

namespace seabattle {

class Luck {
public:
    Luck();

    // A whole number from 0 up to, not including, Count.
    std::uint32_t Below(std::uint32_t Count);
    // From 0 up to, not including, 1.
    float Unit();
    // The moment of something a person did, mixed into the state.
    void Stir(std::uint64_t Moment);

private:
    std::uint64_t Word();
    std::uint64_t Next();

    box::hardware_entropy Source;
    std::uint64_t         State[4];
};

}  // namespace seabattle

#endif
