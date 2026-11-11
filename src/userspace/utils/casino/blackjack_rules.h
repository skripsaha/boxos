/* blackjack_rules.h — the rules of the blackjack table that the person may
 * set (settings casino): read with the rest of the look, and taken for the
 * next round. */
#ifndef CASINO_BLACKJACK_RULES_H
#define CASINO_BLACKJACK_RULES_H

namespace casino {

struct BlackjackRules {
    int  Decks            = 6;
    bool HitSoft17        = false;     // the dealer hits a soft 17 rather than stands
    bool DoubleAny        = true;      // double on any two cards, else on hard 9, 10 and 11
    bool DoubleAfterSplit = true;
    int  MaxHands         = 4;         // the most hands a player makes by splitting
    bool ResplitAces      = false;
    bool Surrender        = false;     // late surrender, once the dealer has looked
    bool Insurance        = true;
};

}  // namespace casino

#endif
