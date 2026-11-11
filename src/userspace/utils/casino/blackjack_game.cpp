/* blackjack_game.cpp — the rules of blackjack (blackjack_game.h). */

#include "blackjack_game.h"

namespace casino {

namespace {

constexpr int Twenty1 = 21;

// A blackjack pays 3 to 2; a half chip is rounded up.
std::int64_t NaturalPay(std::int64_t Bet)
{
    return (Bet * 3 + 1) / 2;
}

}  // namespace

std::int64_t BlackjackGame::Staked() const
{
    std::int64_t Chips = Insured;
    for (const Hand &One : Players) Chips += One.Bet;
    return Chips;
}

std::int64_t BlackjackGame::Returned() const
{
    std::int64_t Chips = Paid;
    for (const Hand &One : Players) Chips += One.Returned;
    return Chips;
}

Dealt BlackjackGame::Take()
{
    return Dealt{Source.Draw(Chances), Serial++};
}

bool BlackjackGame::DealerNatural() const
{
    return House.Cards.size() == 2 && House.Count().Value == Twenty1;
}

bool BlackjackGame::DealerWants() const
{
    const Total Held = House.Count();
    return Held.Value < 17 || (Held.Value == 17 && Held.Soft && Rule.HitSoft17);
}

void BlackjackGame::Begin(std::int64_t Bet, const BlackjackRules &Wanted)
{
    Rule = Wanted;
    if (Source.Decks() != Rule.Decks) Source = Shoe(Rule.Decks);
    Fresh = Source.CutReached();
    if (Fresh) Source.Shuffle(Chances);
    Players.assign(1, Hand{});
    Players[0].Bet = Bet;
    House          = Hand{};
    At             = 0;
    Insured        = 0;
    Paid           = 0;
    Serial         = 0;
    Shown          = false;
    Players[0].Cards.push_back(Take());
    House.Cards.push_back(Take());
    Players[0].Cards.push_back(Take());
    House.Cards.push_back(Take());
    if (IsAce(House.Cards[0].Face) && Rule.Insurance) State = Phase::Insurance;
    else Peek();
}

// The dealer looks at the hole card when an ace or a ten shows; a blackjack
// ends the round at once.
void BlackjackGame::Peek()
{
    const Card Up = House.Cards[0].Face;
    if ((IsAce(Up) || Points(Up) == 10) && DealerNatural()) {
        Finish();
        return;
    }
    if (Players[0].Natural()) {
        Finish();
        return;
    }
    State = Phase::Player;
    At    = 0;
}

bool BlackjackGame::CanInsure() const
{
    return State == Phase::Insurance && Players[0].Bet / 2 > 0;
}

void BlackjackGame::Insure(bool Take)
{
    if (State != Phase::Insurance) return;
    if (Take && CanInsure()) Insured = Players[0].Bet / 2;
    Peek();
}

bool BlackjackGame::CanHit() const
{
    return State == Phase::Player && !Players[At].SplitAces;
}

bool BlackjackGame::CanDouble() const
{
    if (State != Phase::Player) return false;
    const Hand &One = Players[At];
    if (One.Cards.size() != 2 || One.SplitAces || (One.Split && !Rule.DoubleAfterSplit)) return false;
    const Total Held = One.Count();
    return Rule.DoubleAny || (!Held.Soft && Held.Value >= 9 && Held.Value <= 11);
}

bool BlackjackGame::CanSplit() const
{
    if (State != Phase::Player || static_cast<int>(Players.size()) >= Rule.MaxHands) return false;
    const Hand &One = Players[At];
    if (One.Cards.size() != 2 || Points(One.Cards[0].Face) != Points(One.Cards[1].Face)) return false;
    return !One.SplitAces || Rule.ResplitAces;
}

bool BlackjackGame::CanSurrender() const
{
    return State == Phase::Player && Rule.Surrender && Players.size() == 1 && Players[0].Cards.size() == 2 && !Players[0].Split;
}

// A hand with fewer than two cards is dealt its second; a hand that cannot
// take another card, or has no use for one, is done.
void BlackjackGame::Examine()
{
    Hand &One = Players[At];
    if (One.Cards.size() < 2) One.Cards.push_back(Take());
    if (One.Count().Value >= Twenty1) One.Done = true;
    else if (One.SplitAces) {
        const bool Again = IsAce(One.Cards[1].Face) && Rule.ResplitAces && static_cast<int>(Players.size()) < Rule.MaxHands;
        One.Done         = !Again;
    }
}

void BlackjackGame::Advance()
{
    while (Players[At].Done) {
        if (++At >= Players.size()) {
            At = Players.size() - 1;
            Finish();
            return;
        }
        Examine();
    }
}

void BlackjackGame::Hit()
{
    if (!CanHit()) return;
    Players[At].Cards.push_back(Take());
    if (Players[At].Count().Value >= Twenty1) Players[At].Done = true;
    Advance();
}

void BlackjackGame::Stand()
{
    if (State != Phase::Player) return;
    Players[At].Done = true;
    Advance();
}

void BlackjackGame::Double()
{
    if (!CanDouble()) return;
    Hand &One = Players[At];
    One.Bet *= 2;
    One.Doubled = true;
    One.Cards.push_back(Take());
    One.Done = true;
    Advance();
}

void BlackjackGame::Split()
{
    if (!CanSplit()) return;
    Hand Second;
    Second.Bet       = Players[At].Bet;
    Second.Split     = true;
    Second.SplitAces = IsAce(Players[At].Cards[0].Face);
    Second.Cards.push_back(Players[At].Cards[1]);
    Players[At].Cards.pop_back();
    Players[At].Split     = true;
    Players[At].SplitAces = Second.SplitAces;
    Players.insert(Players.begin() + static_cast<std::ptrdiff_t>(At) + 1, Second);
    Examine();
    Advance();
}

void BlackjackGame::Surrender()
{
    if (!CanSurrender()) return;
    Players[0].Result = Outcome::Surrender;
    Players[0].Done   = true;
    Finish();
}

void BlackjackGame::Abandon()
{
    if (State == Phase::Insurance) Insure(false);
    if (State != Phase::Player) return;
    for (At = 0; At < Players.size(); At++) {
        Examine();
        Players[At].Done = true;
    }
    At = Players.size() - 1;
    Finish();
}

// The dealer shows the hole card and plays by the rule if any hand is still in
// the running; then every hand and the insurance are settled.
void BlackjackGame::Finish()
{
    Shown = true;
    const bool Natural = DealerNatural();
    bool       Live    = false;
    for (const Hand &One : Players)
        Live = Live || (One.Result != Outcome::Surrender && One.Count().Value <= Twenty1 && !One.Natural());
    if (Live && !Natural)
        while (DealerWants()) House.Cards.push_back(Take());
    const int Held = House.Count().Value;
    for (Hand &One : Players) Settle(One, Held > Twenty1, Held);
    if (Natural && Insured > 0) Paid = Insured * 3;
    State = Phase::Over;
}

void BlackjackGame::Settle(Hand &One, bool DealerBust, int DealerTotal)
{
    const bool Natural = DealerNatural();
    const int  Held    = One.Count().Value;
    if (One.Result == Outcome::Surrender) {
        One.Returned = One.Bet - One.Bet / 2;
    } else if (Held > Twenty1) {
        One.Result   = Outcome::Bust;
        One.Returned = 0;
    } else if (One.Natural()) {
        One.Result   = Natural ? Outcome::Push : Outcome::Blackjack;
        One.Returned = Natural ? One.Bet : One.Bet + NaturalPay(One.Bet);
    } else if (Natural) {
        One.Result   = Outcome::Lose;
        One.Returned = 0;
    } else if (DealerBust || Held > DealerTotal) {
        One.Result   = Outcome::Win;
        One.Returned = One.Bet * 2;
    } else if (Held == DealerTotal) {
        One.Result   = Outcome::Push;
        One.Returned = One.Bet;
    } else {
        One.Result   = Outcome::Lose;
        One.Returned = 0;
    }
    One.Done = true;
}

}  // namespace casino
