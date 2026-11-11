/* luck.cpp — the casino's draws (luck.h). */

#include "luck.h"

namespace casino {

namespace {

std::uint64_t Split(std::uint64_t &Seed)
{
    Seed += 0x9E3779B97F4A7C15ull;
    std::uint64_t Mixed = Seed;
    Mixed               = (Mixed ^ (Mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    Mixed               = (Mixed ^ (Mixed >> 27)) * 0x94D049BB133111EBull;
    return Mixed ^ (Mixed >> 31);
}

std::uint64_t Rotate(std::uint64_t Value, int By)
{
    return (Value << By) | (Value >> (64 - By));
}

}  // namespace

Luck::Luck()
{
    std::uint64_t Seed = ::cpu_rdtsc();
    for (std::uint64_t &Part : State) Part = Split(Seed);
}

std::uint64_t Luck::Next()
{
    const std::uint64_t Result = Rotate(State[1] * 5, 7) * 9;
    const std::uint64_t Moved  = State[1] << 17;
    State[2] ^= State[0];
    State[3] ^= State[1];
    State[1] ^= State[2];
    State[0] ^= State[3];
    State[2] ^= Moved;
    State[3] = Rotate(State[3], 45);
    return Result;
}

// The time stamp counter is folded into the generator at every draw, so a
// machine without entropy of its own still moves with the clock.
std::uint64_t Luck::Word()
{
    if (box::hardware_entropy::engaged()) return Source();
    std::uint64_t Seed = ::cpu_rdtsc();
    State[0] ^= Split(Seed);
    return Next();
}

std::uint32_t Luck::Below(std::uint32_t Count)
{
    if (Count <= 1) return 0;
    const std::uint64_t Limit = UINT64_MAX - UINT64_MAX % Count;
    std::uint64_t       Drawn = Word();
    while (Drawn >= Limit) Drawn = Word();
    return static_cast<std::uint32_t>(Drawn % Count);
}

float Luck::Unit()
{
    return static_cast<float>(Word() >> 40) / static_cast<float>(1ull << 24);
}

}  // namespace casino
