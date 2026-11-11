/* cards.cpp — playing cards (cards.h). */

#include "cards.h"

namespace casino {

int Points(Card One)
{
    return One.Rank >= 10 ? 10 : One.Rank;
}

bool IsAce(Card One)
{
    return One.Rank == 1;
}

bool IsRed(Suit Of)
{
    return Of == Suit::Hearts || Of == Suit::Diamonds;
}

bool IsCourt(Card One)
{
    return One.Rank >= 11;
}

std::string_view RankName(int Rank)
{
    static constexpr std::string_view Names[RankKinds] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    return Names[(Rank - 1 + RankKinds) % RankKinds];
}

char SuitGlyph(Suit Of)
{
    static constexpr char Glyphs[SuitKinds] = {0x06, 0x03, 0x04, 0x05};
    return Glyphs[static_cast<int>(Of)];
}

}  // namespace casino
