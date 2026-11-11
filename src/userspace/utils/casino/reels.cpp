/* reels.cpp — the strip, the pay table and the arithmetic of both (reels.h). */

#include "reels.h"

namespace casino {

namespace {

// S seven, B bar, L bell, T star, C cherry, M lemon: 2, 3, 4, 5, 6 and 12.
constexpr std::string_view Strip = "SMCMTMLMBMCTMLSMCMTBMLCMTMBLCMTC";
static_assert(Strip.size() == StripLength);

constexpr Symbol Seven  = Symbol::Seven;
constexpr Symbol Bar    = Symbol::Bar;
constexpr Symbol Bell   = Symbol::Bell;
constexpr Symbol Star   = Symbol::Star;
constexpr Symbol Cherry = Symbol::Cherry;
constexpr Symbol Lemon  = Symbol::Lemon;

constexpr Pay Table[] = {
    {"three sevens: the jackpot", {Seven, Seven, Seven}, {false, false, false}, 500},
    {"three bars", {Bar, Bar, Bar}, {false, false, false}, 100},
    {"three bells", {Bell, Bell, Bell}, {false, false, false}, 40},
    {"three stars", {Star, Star, Star}, {false, false, false}, 20},
    {"three cherries", {Cherry, Cherry, Cherry}, {false, false, false}, 10},
    {"three lemons", {Lemon, Lemon, Lemon}, {false, false, false}, 6},
    {"two cherries on the left", {Cherry, Cherry, Cherry}, {false, false, true}, 2},
    {"a cherry on the left", {Cherry, Cherry, Cherry}, {false, true, true}, 1},
};

constexpr Line Lines[MostLines] = {{{1, 1, 1}}, {{0, 0, 0}}, {{2, 2, 2}}, {{0, 1, 2}}, {{2, 1, 0}}};

// The first row of the pay table the three symbols fit.
const Pay *Paying(const Symbol Seen[ReelCount])
{
    for (const Pay &Row : Table) {
        bool Fits = true;
        for (int Reel = 0; Reel < ReelCount && Fits; Reel++) Fits = Row.Any[Reel] || Seen[Reel] == Row.Needs[Reel];
        if (Fits) return &Row;
    }
    return nullptr;
}

Symbol FromLetter(char Letter)
{
    switch (Letter) {
    case 'S': return Symbol::Seven;
    case 'B': return Symbol::Bar;
    case 'L': return Symbol::Bell;
    case 'T': return Symbol::Star;
    case 'C': return Symbol::Cherry;
    default:  return Symbol::Lemon;
    }
}

}  // namespace

Symbol OnStrip(int Stop)
{
    return FromLetter(Strip[static_cast<std::size_t>(((Stop % StripLength) + StripLength) % StripLength)]);
}

std::string_view NameOf(Symbol Which)
{
    static constexpr std::string_view Names[SymbolKinds] = {"seven", "bar", "bell", "star", "cherry", "lemon"};
    return Names[static_cast<int>(Which)];
}

Window WindowAt(std::span<const int, ReelCount> Stops)
{
    Window Seen;
    for (int Reel = 0; Reel < ReelCount; Reel++)
        for (int Row = 0; Row < WindowRows; Row++) Seen.Seen[Reel][Row] = OnStrip(Stops[Reel] + 1 - Row);
    return Seen;
}

const Line &LineAt(int Which)
{
    return Lines[Which];
}

std::span<const Pay> PayTable()
{
    return Table;
}

std::vector<Win> Evaluate(const Window &Seen, int Played)
{
    std::vector<Win> Wins;
    for (int At = 0; At < Played && At < MostLines; At++) {
        Symbol OnLine[ReelCount];
        for (int Reel = 0; Reel < ReelCount; Reel++) OnLine[Reel] = Seen.Seen[Reel][Lines[At].Row[Reel]];
        const Pay *Row = Paying(OnLine);
        if (!Row) continue;
        int Made = 0;
        while (Made < ReelCount && !Row->Any[Made]) Made++;
        Wins.push_back(Win{At, Row->Multiple, Row == &Table[0], Made});
    }
    return Wins;
}

double ReturnToPlayer()
{
    long Total = 0;
    for (int First = 0; First < StripLength; First++)
        for (int Second = 0; Second < StripLength; Second++)
            for (int Third = 0; Third < StripLength; Third++) {
                const Symbol Seen[ReelCount] = {OnStrip(First), OnStrip(Second), OnStrip(Third)};
                if (const Pay *Row = Paying(Seen)) Total += Row->Multiple;
            }
    return static_cast<double>(Total) / (StripLength * StripLength * StripLength);
}

int PayingCombinations()
{
    int Count = 0;
    for (int First = 0; First < StripLength; First++)
        for (int Second = 0; Second < StripLength; Second++)
            for (int Third = 0; Third < StripLength; Third++) {
                const Symbol Seen[ReelCount] = {OnStrip(First), OnStrip(Second), OnStrip(Third)};
                if (Paying(Seen)) Count++;
            }
    return Count;
}

int JackpotCombinations()
{
    int Count = 0;
    for (int First = 0; First < StripLength; First++)
        for (int Second = 0; Second < StripLength; Second++)
            for (int Third = 0; Third < StripLength; Third++) {
                const Symbol Seen[ReelCount] = {OnStrip(First), OnStrip(Second), OnStrip(Third)};
                if (Paying(Seen) == &Table[0]) Count++;
            }
    return Count;
}

}  // namespace casino
