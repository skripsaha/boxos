/* blackjack_game.h — the rules of blackjack, with no picture and no purse.
 *
 * A round goes: the bet, two cards each (the dealer's second face down), the
 * dealer looks for a blackjack when the card showing is an ace or a ten, and
 * insurance is offered against an ace. The player then plays each hand in
 * turn: hit, stand, double, split, or surrender where the rules allow; the
 * dealer plays by the rule and every hand is settled. A blackjack pays 3 to 2,
 * a win 1 to 1, insurance 2 to 1; where a payout would be half a chip it is
 * rounded in the player's favour. A split hand of aces takes one card and
 * stops, and 21 on a split hand is no blackjack.
 *
 * The game only counts: the player's chips are the table's business. The
 * table asks Can... first and takes the stake of Double, Split and Insure
 * before it calls them. */
#ifndef CASINO_BLACKJACK_GAME_H
#define CASINO_BLACKJACK_GAME_H

#include <cstdint>
#include <vector>

#include "blackjack_rules.h"
#include "cards.h"
#include "hands.h"
#include "luck.h"

namespace casino {

enum class Phase : std::uint8_t { Betting, Insurance, Player, Over };

class BlackjackGame {
public:
    explicit BlackjackGame(Luck &Chance) : Chances(Chance), Source(6) {}

    Phase                    Now() const { return State; }
    const std::vector<Hand> &Hands() const { return Players; }
    const Hand              &Dealer() const { return House; }
    std::size_t              Active() const { return At; }
    // The dealer's second card is face up: no longer hidden.
    bool                     HoleShown() const { return Shown; }
    const Shoe              &Stock() const { return Source; }
    // The round began on a freshly shuffled shoe.
    bool                     Reshuffled() const { return Fresh; }
    const BlackjackRules    &Rules() const { return Rule; }
    // How many cards have come out of the shoe this round.
    std::uint32_t            Drawn() const { return Serial; }

    // The chips staked this round, insurance included, and what came back.
    std::int64_t Staked() const;
    std::int64_t Returned() const;
    std::int64_t InsuredFor() const { return Insured; }

    // Deals a round for Bet under the rules wanted. Phase is then Insurance, Player or
    // Over (a blackjack was dealt to someone).
    void Begin(std::int64_t Bet, const BlackjackRules &Wanted);

    bool CanInsure() const;
    // The insurance costs half the bet; Take false declines it.
    void Insure(bool Take);

    bool CanHit() const;
    bool CanDouble() const;
    bool CanSplit() const;
    bool CanSurrender() const;
    void Hit();
    void Stand();
    void Double();
    void Split();
    void Surrender();
    // The player leaves in the middle of a round: every open hand stands.
    void Abandon();

private:
    Dealt Take();
    bool  DealerNatural() const;
    bool  DealerWants() const;
    void  Peek();
    void  Examine();
    void  Advance();
    void  Finish();
    void  Settle(Hand &One, bool DealerBust, int DealerTotal);

    Luck                  &Chances;
    Shoe                   Source;
    BlackjackRules         Rule;
    Phase                  State   = Phase::Betting;
    std::vector<Hand>      Players;
    Hand                   House;
    std::size_t            At      = 0;
    std::int64_t           Insured = 0;
    std::int64_t           Paid    = 0;      // what insurance gave back
    std::uint32_t          Serial  = 0;
    bool                   Shown   = false;
    bool                   Fresh   = false;
};

}  // namespace casino

#endif
