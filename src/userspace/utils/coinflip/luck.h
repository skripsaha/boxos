/* luck.h — which way the coin falls. One draw of the processor's own entropy
 * (RDSEED, else RDRAND) decides each flip, by its top bit. A machine with
 * neither falls back to SplitMix64 kept moving by the clock — its state takes
 * the time stamp counter at every draw — and says so: Honest is false then. */
#ifndef COINFLIP_LUCK_H
#define COINFLIP_LUCK_H

#include <cstdint>

#include "box/cpu.h"
#include "box/cxx/cpu.h"

#include "toss.h"

namespace coinflip {

class Luck {
public:
    Luck() : Clocked(::cpu_rdtsc()) {}

    // Whether the draws come from the processor's entropy, not from the clock.
    static bool Honest() { return box::hardware_entropy::engaged(); }

    Face Draw() { return (Word() >> 63) != 0 ? Face::Tails : Face::Heads; }

private:
    std::uint64_t Word()
    {
        if (Honest()) return Source();
        Clocked += 0x9E3779B97F4A7C15ull + ::cpu_rdtsc();
        std::uint64_t Mixed = Clocked;
        Mixed               = (Mixed ^ (Mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        Mixed               = (Mixed ^ (Mixed >> 27)) * 0x94D049BB133111EBull;
        return Mixed ^ (Mixed >> 31);
    }

    box::hardware_entropy Source;
    std::uint64_t         Clocked;
};

}  // namespace coinflip

#endif
