/* hands.cpp — the count of a hand (hands.h). */

#include "hands.h"

namespace casino {

Total TotalOf(const std::vector<Dealt> &Cards)
{
    int  Hard = 0;
    bool Ace  = false;
    for (const Dealt &One : Cards) {
        Hard += Points(One.Face);
        Ace = Ace || IsAce(One.Face);
    }
    if (Ace && Hard + 10 <= 21) return Total{Hard + 10, true};
    return Total{Hard, false};
}

bool Hand::Natural() const
{
    return !Split && Cards.size() == 2 && Count().Value == 21;
}

}  // namespace casino
