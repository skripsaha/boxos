/* round.cpp — the rules of one word (round.h). */

#include "round.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace hangman {

Round::Round(std::string Word, std::string Label, Level Hardness)
    : Secret(std::move(Word)), Called(std::move(Label)), Hard(Hardness)
{
}

bool Round::IsLetter(char Raw)
{
    return (Raw >= 'a' && Raw <= 'z') || (Raw >= 'A' && Raw <= 'Z');
}

char Round::Upper(char Raw)
{
    return Raw >= 'a' && Raw <= 'z' ? static_cast<char>(Raw - 'a' + 'A') : Raw;
}

Guess Round::Say(char Letter)
{
    Letter = Upper(Letter);
    if (Standing != Phase::Playing || !IsLetter(Letter) || Tried(Letter)) return Guess::Repeated;
    if (Secret.find(Letter) != std::string::npos) {
        Hit.set(Index(Letter));
        Settle();
        return Guess::Right;
    }
    Miss.set(Index(Letter));
    Order += Letter;
    Faults++;
    Settle();
    return Guess::Wrong;
}

void Round::Settle()
{
    if (Faults >= MostMistakes) Standing = Phase::Lost;
    else if (Hidden() == 0) Standing = Phase::Won;
}

int Round::Hidden() const
{
    std::bitset<Letters> Lacking;
    for (const char Letter : Secret)
        if (!Found(Letter)) Lacking.set(Index(Letter));
    return static_cast<int>(Lacking.count());
}

std::optional<char> Round::Reveal(Luck &Chance)
{
    if (Standing != Phase::Playing) return std::nullopt;
    std::vector<char> Candidates;
    for (const char Letter : Secret)
        if (!Found(Letter) && std::find(Candidates.begin(), Candidates.end(), Letter) == Candidates.end()) Candidates.push_back(Letter);
    if (Candidates.empty()) return std::nullopt;
    const char Given = Candidates[Chance.Below(static_cast<std::uint32_t>(Candidates.size()))];
    Hit.set(Index(Given));
    Helped++;
    Settle();
    return Given;
}

bool Round::Pay()
{
    if (Standing != Phase::Playing || Faults + 1 >= MostMistakes) return false;
    Faults++;
    return true;
}

void Round::Concede()
{
    if (Standing == Phase::Playing) Standing = Phase::Lost;
}

}  // namespace hangman
