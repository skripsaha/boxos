/* source.h — where a roll's randomness comes from: the chip's own entropy
 * when it has any, RDSEED first and RDRAND after it, and when it has none or
 * stops answering, a software generator kept moving by the cycle counter. The
 * weakest one that gave a draw is said, never claimed better than it is. */
#ifndef DICE_SOURCE_H
#define DICE_SOURCE_H

#include <cstdint>

namespace dice {

enum class Entropy : std::uint8_t { Seed, Rand, Software };

class Source {
public:
    Source();

    std::uint64_t Next();
    // A number from 0 to Limit - 1, every one as likely as any other: a draw
    // that falls in the short run at the bottom of the range is thrown away,
    // not folded back by a remainder. Limit must be 1 or more.
    std::uint64_t Below(std::uint64_t Limit);
    Entropy       Used() const { return Weakest; }

private:
    bool          Hardware(std::uint64_t &Word);
    std::uint64_t Software();

    std::uint64_t State[4];
    Entropy       Weakest;
};

const char *Name(Entropy Kind);

}  // namespace dice

#endif
