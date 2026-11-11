/* bets.h — what can be bet on at the roulette table, and what each bet pays.
 * Every bet covers some of the 37 numbers and pays 36 for each chip over the
 * numbers it covers, the stake included: 35 to 1 on one number, 17 to 1 on
 * two, 2 to 1 on twelve, evens on eighteen. The zero is lost to the house on
 * every bet but its own, which is the house's whole edge, 1 in 37. */
#ifndef CASINO_BETS_H
#define CASINO_BETS_H

#include <cstdint>
#include <string>
#include <vector>

namespace casino {

enum class Kind : std::uint8_t { Straight, Split, Column, Dozen, Low, High, Even, Odd, Red, Black };

// A place to put chips. A and B are the numbers of a straight or a split, or
// the column or dozen (0 to 2).
struct Spot {
    Kind         What = Kind::Straight;
    std::uint8_t A    = 0;
    std::uint8_t B    = 0;

    bool operator==(const Spot &) const = default;
};

bool        Covers(const Spot &On, int Number);
int         Covered(const Spot &On);
// What a chip on it comes back as when it wins, the chip included.
int         Return(const Spot &On);
std::string Called(const Spot &On);

struct Placed {
    Spot         On;
    std::int64_t Chips = 0;
};

class Bets {
public:
    void         Add(const Spot &On, std::int64_t Chips);
    // Up to Chips taken back from the spot; how many were.
    std::int64_t Take(const Spot &On, std::int64_t Chips);
    std::int64_t On(const Spot &Where) const;
    std::int64_t Total() const;
    bool         Empty() const { return Laid.empty(); }
    void         Clear() { Laid.clear(); }
    // What the bets come back as when Number comes up, the stakes included.
    std::int64_t Returned(int Number) const;
    const std::vector<Placed> &All() const { return Laid; }

private:
    std::vector<Placed> Laid;
};

}  // namespace casino

#endif
