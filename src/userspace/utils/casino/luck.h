/* luck.h — every chance of the casino. The processor's own entropy (RDSEED,
 * else RDRAND) where it has any; a machine with neither falls back to
 * xoshiro256** seeded through SplitMix64 from the time stamp counter and kept
 * moving by it. Ranges are unbiased. */
#ifndef CASINO_LUCK_H
#define CASINO_LUCK_H

#include <cstdint>

#include "box/cpu.h"
#include "box/cxx/cpu.h"

namespace casino {

class Luck {
public:
    Luck();

    // A whole number from 0 up to, not including, Count.
    std::uint32_t Below(std::uint32_t Count);
    // From 0 up to, not including, 1.
    float Unit();
    // From Low to High.
    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }

private:
    std::uint64_t Word();
    std::uint64_t Next();

    box::hardware_entropy Source;
    std::uint64_t         State[4];
};

}  // namespace casino

#endif
