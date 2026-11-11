/* source.cpp — the draws. */

#include "source.h"

#include "box/clock.h"
#include "box/cpu.h"

namespace dice {

namespace {

// RDRAND may fail a few times in a row under load, and Intel says to try ten
// times; RDSEED fails more often, so it gets more tries before the next source.
constexpr int RandTries = 10;
constexpr int SeedTries = 32;

std::uint64_t Splitmix(std::uint64_t &Seed)
{
    std::uint64_t Word = (Seed += 0x9E3779B97F4A7C15ull);
    Word               = (Word ^ (Word >> 30)) * 0xBF58476D1CE4E5B9ull;
    Word               = (Word ^ (Word >> 27)) * 0x94D049BB133111EBull;
    return Word ^ (Word >> 31);
}

std::uint64_t Rotate(std::uint64_t Word, int By)
{
    return (Word << By) | (Word >> (64 - By));
}

}  // namespace

Source::Source() : Weakest(Entropy::Seed)
{
    if (!cpu_has_rdseed()) Weakest = cpu_has_rdrand() ? Entropy::Rand : Entropy::Software;
    std::uint64_t Seed = cpu_rdtsc() ^ (clock_uptime_us() << 32);
    for (std::uint64_t &Word : State) Word = Splitmix(Seed) ^ cpu_rdtsc();
}

bool Source::Hardware(std::uint64_t &Word)
{
    if (cpu_has_rdseed()) {
        for (int Try = 0; Try < SeedTries; Try++)
            if (cpu_rdseed64(&Word)) return true;
    }
    if (cpu_has_rdrand()) {
        for (int Try = 0; Try < RandTries; Try++) {
            if (cpu_rdrand64(&Word)) {
                if (Weakest == Entropy::Seed) Weakest = Entropy::Rand;
                return true;
            }
        }
    }
    return false;
}

// xoshiro256**, stirred with a fresh cycle count at every draw so that two
// draws are never the same function of the start alone.
std::uint64_t Source::Software()
{
    State[0] ^= cpu_rdtsc();
    const std::uint64_t Result = Rotate(State[1] * 5, 7) * 9;
    const std::uint64_t Shift  = State[1] << 17;
    State[2] ^= State[0];
    State[3] ^= State[1];
    State[1] ^= State[2];
    State[0] ^= State[3];
    State[2] ^= Shift;
    State[3] = Rotate(State[3], 45);
    Weakest  = Entropy::Software;
    return Result;
}

std::uint64_t Source::Next()
{
    std::uint64_t Word = 0;
    if (Hardware(Word)) return Word;
    return Software();
}

std::uint64_t Source::Below(std::uint64_t Limit)
{
    // 2^64 mod Limit of the draws are the ones that would make low numbers likelier.
    const std::uint64_t Short = (0 - Limit) % Limit;
    std::uint64_t       Word  = Next();
    while (Word < Short) Word = Next();
    return Word % Limit;
}

const char *Name(Entropy Kind)
{
    switch (Kind) {
    case Entropy::Seed: return "RDSEED";
    case Entropy::Rand: return "RDRAND";
    default:            return "software";
    }
}

}  // namespace dice
