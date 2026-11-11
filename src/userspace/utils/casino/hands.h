/* hands.h — what a player or the dealer holds in blackjack: the cards dealt,
 * what they count, and how the hand ended. */
#ifndef CASINO_HANDS_H
#define CASINO_HANDS_H

#include <cstdint>
#include <vector>

#include "cards.h"

namespace casino {

// A card as it came out of the shoe: Serial counts the round's draws, in order.
struct Dealt {
    Card          Face;
    std::uint32_t Serial = 0;
};

// What cards count: the best total not over 21 where there is one, and
// whether an ace is still counted as 11 in it.
struct Total {
    int  Value = 0;
    bool Soft  = false;
};

Total TotalOf(const std::vector<Dealt> &Cards);

enum class Outcome : std::uint8_t { Open, Blackjack, Win, Push, Lose, Bust, Surrender };

struct Hand {
    std::vector<Dealt> Cards;
    std::int64_t       Bet        = 0;
    std::int64_t       Returned   = 0;      // what the hand gave back, the bet included
    Outcome            Result     = Outcome::Open;
    bool               Doubled    = false;
    bool               Split      = false;  // came out of a split
    bool               SplitAces  = false;  // came out of a split of aces
    bool               Done       = false;

    Total Count() const { return TotalOf(Cards); }
    // Two cards, 21, and not a hand of a split.
    bool Natural() const;
};

}  // namespace casino

#endif
