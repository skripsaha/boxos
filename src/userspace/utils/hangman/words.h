/* words.h — the words hangman knows, grouped by category, and how hard each is:
 * by its length, and by the rare letters in it (j q x z k v), which are the ones
 * a guesser reaches last. A Deck draws one by chance and does not repeat what it
 * drew a moment ago. */
#ifndef HANGMAN_WORDS_H
#define HANGMAN_WORDS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "luck.h"

namespace hangman {

enum class Level : std::uint8_t { Easy, Medium, Hard };

struct Category {
    std::string_view                  Name;
    std::span<const std::string_view> Words;
};

std::span<const Category> Categories();

// The category called Name, or Categories().size() when there is none.
std::size_t CategoryNamed(std::string_view Name);

// How hard a word of capital letters is to guess.
Level LevelOf(std::string_view Word);
std::string_view LevelName(Level Which);
// The level called Name, if it is one.
std::optional<Level> LevelNamed(std::string_view Name);

struct Drawn {
    std::string_view Word;
    std::size_t      Category;
};

class Deck {
public:
    // One word by chance, from Only (every category when none) at Hardness (every level when none).
    // Nothing when no word fits.
    std::optional<Drawn> Draw(Luck &Chance, std::optional<std::size_t> Only, std::optional<Level> Hardness);

private:
    bool Recent(std::string_view Word) const;

    std::vector<std::string_view> Seen;
};

}  // namespace hangman

#endif
