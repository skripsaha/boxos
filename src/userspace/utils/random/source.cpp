/* source.cpp — the hardware generator and the one that stands in for it. */

#include "source.h"

#include "box/cpu.h"
#include "box/time.h"

namespace random_pick {

namespace {

std::uint64_t Mixed(std::uint64_t &Seed) noexcept
{
    Seed += 0x9E3779B97F4A7C15ull;
    std::uint64_t Word = Seed;
    Word               = (Word ^ (Word >> 30)) * 0xBF58476D1CE4E5B9ull;
    Word               = (Word ^ (Word >> 27)) * 0x94D049BB133111EBull;
    return Word ^ (Word >> 31);
}

std::uint64_t Rotated(std::uint64_t Word, int By) noexcept
{
    return (Word << By) | (Word >> (64 - By));
}

// What the machine cannot repeat, as one number.
std::uint64_t Unrepeatable() noexcept
{
    std::uint64_t Seed   = cpu_rdtsc();
    std::uint64_t Uptime = 0;
    time_uptime_ns(&Uptime);
    Seed ^= Rotated(Uptime, 21);
    Seed ^= reinterpret_cast<std::uintptr_t>(&Seed);
    std::uint64_t Word = 0;
    if (cpu_has_rdseed() && cpu_rdseed64(&Word)) Seed ^= Word;
    return Seed;
}

}  // namespace

Source::Source() noexcept : State{}
{
    std::uint64_t Seed = Unrepeatable();
    for (std::uint64_t &Part : State) Part = Mixed(Seed) ^ cpu_rdtsc();
}

std::uint64_t Source::Fallback() noexcept
{
    const std::uint64_t Result = Rotated(State[1] * 5, 7) * 9;
    const std::uint64_t Moved  = State[1] << 17;
    State[2] ^= State[0];
    State[3] ^= State[1];
    State[1] ^= State[2];
    State[0] ^= State[3];
    State[2] ^= Moved;
    State[3] = Rotated(State[3], 45);
    return Result;
}

std::uint64_t Source::Next() noexcept
{
    std::uint64_t Word = 0;
    if (cpu_has_rdrand() && cpu_rdrand64(&Word)) return Word;
    return Fallback();
}

std::uint64_t Source::Below(std::uint64_t Limit) noexcept
{
    const std::uint64_t Wasted = (0 - Limit) % Limit;
    std::uint64_t       Word   = Next();
    while (Word < Wasted) Word = Next();
    return Word % Limit;
}

}  // namespace random_pick
