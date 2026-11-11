/* purse.cpp — the purse and its file (purse.h). */

#include "purse.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <string_view>

#include "keep.h"

namespace casino {

namespace {

const Keep PurseFile{"casino.purse", "casino,purse"};

// The number after Word on a line of Text; Fallback where there is none.
std::int64_t After(std::string_view Text, std::string_view Word, std::int64_t Fallback)
{
    const std::string Marked = std::format("{} ", Word);
    std::size_t       At     = Text.find(Marked);
    while (At != std::string_view::npos && At != 0 && Text[At - 1] != '\n') At = Text.find(Marked, At + 1);
    if (At == std::string_view::npos) return Fallback;
    std::int64_t Value = Fallback;
    const char  *From  = Text.data() + At + Marked.size();
    std::from_chars(From, Text.data() + Text.size(), Value);
    return Value;
}

}  // namespace

Purse::Purse(const Look &Shown)
{
    std::string Text;
    if (const std::optional<std::string> Refusal = KeepRead(PurseFile, Text)) {
        Said     = *Refusal;
        Writable = false;
    }
    Kept.Chips   = After(Text, "chips", Shown.StartChips);
    Kept.Biggest  = After(Text, "biggest", 0);
    Kept.Jackpots = After(Text, "jackpots", 0);
    Kept.Rounds   = After(Text, "rounds", 0);
    Kept.Wins     = After(Text, "wins", 0);
    Opening       = Kept.Chips;
}

bool Purse::Take(std::int64_t Chips)
{
    if (Chips <= 0 || Chips > Kept.Chips) return false;
    Kept.Chips -= Chips;
    OnTable += Chips;
    return true;
}

void Purse::TakeBack(std::int64_t Chips)
{
    Kept.Chips += Chips;
    OnTable -= Chips;
}

void Purse::Settle(std::int64_t Staked, std::int64_t Returned, bool Jackpot)
{
    OnTable -= Staked;
    Kept.Chips += Returned;
    Kept.Rounds++;
    if (Returned > Staked) Kept.Wins++;
    if (Returned - Staked > Kept.Biggest) Kept.Biggest = Returned - Staked;
    if (Jackpot) Kept.Jackpots++;
    Save();
}

std::optional<std::string> Purse::Refilled(const Look &Shown)
{
    if (!Shown.Refill || OnTable != 0 || Kept.Chips >= Shown.Step) return std::nullopt;
    const std::int64_t Stack = std::max(Shown.RefillChips, Shown.Step);
    Kept.Chips += Stack;
    Opening += Stack;
    Save();
    return std::format("The house slides you a fresh stack: {} chips.", Stack);
}

void Purse::Save()
{
    if (!Writable) return;
    const std::string Text = std::format("chips {}\nbiggest {}\njackpots {}\nrounds {}\nwins {}\n", Kept.Chips + OnTable,
                                         Kept.Biggest, Kept.Jackpots, Kept.Rounds, Kept.Wins);
    if (const std::optional<std::string> Refusal = KeepWrite(PurseFile, Text)) Said = *Refusal;
    else Said.clear();
}

}  // namespace casino
