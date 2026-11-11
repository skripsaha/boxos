/* round.h — one word being guessed: the letters tried, the mistakes, the hints
 * taken and how it stands. The rules and nothing of the picture. Six mistakes
 * lose the round; a word whose letters are all found wins it. */
#ifndef HANGMAN_ROUND_H
#define HANGMAN_ROUND_H

#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "luck.h"
#include "words.h"

namespace hangman {

inline constexpr int MostMistakes = 6;
inline constexpr int Letters = 26;

enum class Phase : std::uint8_t { Playing, Won, Lost };

// What a letter said by the person came to.
enum class Guess : std::uint8_t { Repeated, Right, Wrong };

class Round {
public:
    // Word is capital letters; Label is what the person is told it is (a category, or a friend's word).
    Round(std::string Word, std::string Label, Level Hardness);

    Guess Say(char Letter);
    // A letter still hidden is given away, chosen by chance; nothing when none is left.
    std::optional<char> Reveal(Luck &Chance);
    // A mistake taken as the price of a hint; false when it would be the last.
    bool Pay();
    // The round given up.
    void Concede();

    const std::string &Word() const { return Secret; }
    const std::string &Label() const { return Called; }
    Level              Hardness() const { return Hard; }
    Phase              State() const { return Standing; }
    int                Mistakes() const { return Faults; }
    int                Hints() const { return Helped; }
    bool               Found(char Letter) const { return Hit.test(Index(Letter)); }
    bool               Failed(char Letter) const { return Miss.test(Index(Letter)); }
    bool               Tried(char Letter) const { return Found(Letter) || Failed(Letter); }
    // Whether the letter at Place in the word has been found.
    bool               Shown(std::size_t Place) const { return Found(Secret[Place]); }
    // Whether a letter has been said or given.
    bool               Touched() const { return Hit.any() || Miss.any(); }
    // How many different letters of the word are still hidden.
    int                Hidden() const;
    // The wrong letters in the order they were said.
    const std::string &Misses() const { return Order; }

    static bool IsLetter(char Raw);
    static char Upper(char Raw);

private:
    static std::size_t Index(char Letter) { return static_cast<std::size_t>(Letter - 'A'); }
    void               Settle();

    std::string          Secret;
    std::string          Called;
    std::string          Order;
    Level                Hard;
    Phase                Standing = Phase::Playing;
    std::bitset<Letters> Hit;
    std::bitset<Letters> Miss;
    int                  Faults = 0;
    int                  Helped = 0;
};

}  // namespace hangman

#endif
