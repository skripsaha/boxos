/* tally.cpp — the counting. */

#include "tally.h"

#include <algorithm>

namespace coinflip {

namespace {

// How many of the last flips are kept to be shown.
constexpr std::size_t RecentKept = 512;

}  // namespace

void Tally::Add(Face Landed, std::optional<Face> Called)
{
    (Landed == Face::Heads ? Heads : Tails)++;
    RunCount = RunCount > 0 && Run == Landed ? RunCount + 1 : 1;
    Run      = Landed;
    std::uint64_t &Longest = Landed == Face::Heads ? LongestHeads : LongestTails;
    Longest                = std::max(Longest, RunCount);
    if (Called) (*Called == Landed ? CallsWon : CallsLost)++;
    if (Last.size() == RecentKept) Last.erase(0, 1);
    Last += Landed == Face::Heads ? 'H' : 'T';
}

}  // namespace coinflip
