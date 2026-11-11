/* cards.h — playing cards and the shoe they are dealt from.
 *
 * The shoe holds whole decks, is shuffled by Fisher-Yates on the casino's own
 * unbiased draws, and has a cut card: once it is reached the round in play is
 * finished and the shoe is shuffled again before the next one. */
#ifndef CASINO_CARDS_H
#define CASINO_CARDS_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace casino {

enum class Suit : std::uint8_t { Spades, Hearts, Diamonds, Clubs };

inline constexpr int SuitKinds = 4;
inline constexpr int RankKinds = 13;
inline constexpr int DeckSize  = SuitKinds * RankKinds;

// Rank is 1 for the ace, 2 to 10 as they are, 11 jack, 12 queen, 13 king.
struct Card {
    std::uint8_t Rank = 1;
    Suit         Of   = Suit::Spades;

    bool operator==(const Card &) const = default;
};

// Spots on the card counted hard: the ace is 1, the court cards 10.
int  Points(Card One);
bool IsAce(Card One);
bool IsRed(Suit Of);
bool IsCourt(Card One);
// "A", "2" to "10", "J", "Q", "K".
std::string_view RankName(int Rank);
// The suit as one byte of the console font (CP437): spade, heart, diamond, club.
char SuitGlyph(Suit Of);

class Shoe {
public:
    explicit Shoe(int Decks) : Count(Decks < 1 ? 1 : Decks) {}

    int Decks() const { return Count; }
    // Cards still to deal, and how many the shoe held when it was shuffled.
    int Left() const { return static_cast<int>(Cards.size()) - Next; }
    int Held() const { return static_cast<int>(Cards.size()); }
    int CutAt() const { return Cut; }
    // The cut card has come out (a shoe not yet shuffled is the same): no new
    // round begins on this shoe.
    bool CutReached() const { return Next >= Cut; }

    // All the decks back in, shuffled, the cut card put 60 to 75 percent of the
    // way in. Chance supplies Below(n): a whole number from 0 to n - 1.
    template <class Chance>
    void Shuffle(Chance &Luck)
    {
        Cards.clear();
        for (int Deck = 0; Deck < Count; Deck++)
            for (int Suit = 0; Suit < SuitKinds; Suit++)
                for (int Rank = 1; Rank <= RankKinds; Rank++)
                    Cards.push_back(Card{static_cast<std::uint8_t>(Rank), static_cast<enum Suit>(Suit)});
        for (std::size_t At = Cards.size() - 1; At > 0; At--)
            std::swap(Cards[At], Cards[Luck.Below(static_cast<std::uint32_t>(At + 1))]);
        Next            = 0;
        const int Size = static_cast<int>(Cards.size());
        Cut            = Size * 60 / 100 + static_cast<int>(Luck.Below(static_cast<std::uint32_t>(Size * 15 / 100 + 1)));
    }

    // The next card. A shoe dealt to its very end is shuffled whole again.
    template <class Chance>
    Card Draw(Chance &Luck)
    {
        if (Next >= static_cast<int>(Cards.size())) Shuffle(Luck);
        return Cards[static_cast<std::size_t>(Next++)];
    }

private:
    int               Count;
    int               Next = 0;
    int               Cut  = 0;
    std::vector<Card> Cards;
};

}  // namespace casino

#endif
