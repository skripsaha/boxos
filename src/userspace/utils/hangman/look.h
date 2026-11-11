/* look.h — what the person set of hangman (settings hangman): read whole, and
 * read again when the file is saved or the Use Context changes. */
#ifndef HANGMAN_LOOK_H
#define HANGMAN_LOOK_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "key_chord.h"

#include "numbers.h"
#include "words.h"

namespace hangman {

// The keys a person chooses, in the order of Look::Keys. Letters are the guesses, so these are
// chords; esc leaves and enter goes on, as they are fixed.
enum HangKey : std::uint8_t { KeyHint, KeyFresh, KeyFriend, KeyHelp, KeyCount };

// What a hint costs: points off the score of the word, or one of the six mistakes.
enum class HintCost : std::uint8_t { Points, Mistake };

struct Look {
    std::optional<std::size_t> Only;         // the one category words come from; none for all
    std::optional<Level>       Hardness;     // the one level; none for all
    std::uint32_t              HintsAllowed; // hints a word allows
    HintCost                   Price;
    bool                       Qwerty;       // the alphabet laid out as a keyboard, else a to z
    Color                      Sky;
    Color                      Grass;
    Color                      Wood;
    Color                      Figure;
    Color                      Sun;
    Color                      Bars;         // the lines at the top and the bottom
    Color                      Title;
    Color                      Text;
    Color                      Quiet;
    Color                      Good;
    Color                      Bad;
    KeyChord                   Keys[KeyCount];
};

Look ReadLook();

// What a hint takes off a word's score when it is paid for in points.
inline constexpr std::uint32_t HintPoints = 25;

}  // namespace hangman

#endif
