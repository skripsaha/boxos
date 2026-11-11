/* source.h — where the randomness comes from: the processor's own generator
 * (RDRAND) where it has one, else a generator seeded from what the machine
 * cannot repeat — the time-stamp counter, the uptime and where the stack
 * landed. The second is fair for a game and a choice, and no more: it is not
 * for secrets. */
#ifndef RANDOM_SOURCE_H
#define RANDOM_SOURCE_H

#include <cstdint>

namespace random_pick {

class Source {
public:
    Source() noexcept;

    std::uint64_t Next() noexcept;

    // A number from 0 up to but not including Limit, every one as likely as
    // every other: what a remainder would favour is drawn again. Limit is not 0.
    std::uint64_t Below(std::uint64_t Limit) noexcept;

private:
    std::uint64_t Fallback() noexcept;

    std::uint64_t State[4];
};

}  // namespace random_pick

#endif
