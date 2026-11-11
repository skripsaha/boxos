/* luck.h — every chance of hangman: which word comes up, which letter a hint
 * gives, where the leaves and the confetti fly. The processor's own entropy
 * (RDSEED, else RDRAND) where it has any; a machine with neither falls back to
 * xoshiro256** seeded through SplitMix64 from the time stamp counter and the
 * clock, and stirred by whatever the person does: the instant of every key is
 * folded in. Ranges are unbiased. */
#ifndef HANGMAN_LUCK_H
#define HANGMAN_LUCK_H

#include <cstdint>

#include "box/cpu.h"
#include "box/cxx/cpu.h"

namespace hangman {

class Luck {
public:
    Luck();

    // A whole number from 0 up to, not including, Count.
    std::uint32_t Below(std::uint32_t Count);
    // From 0 up to, not including, 1.
    float Unit();
    // From Low up to, not including, High.
    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }
    // The moment of something a person did, mixed into the state.
    void Stir(std::uint64_t Moment);

private:
    std::uint64_t Word();
    std::uint64_t Next();

    box::hardware_entropy Source;
    std::uint64_t         State[4];
};

}  // namespace hangman

#endif
