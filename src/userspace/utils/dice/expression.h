/* expression.h — what is to be rolled, as a person writes it: terms added
 * and taken away, each a group of dice or a plain number.
 *
 *   d20   3d6   d%   2d6+3   d20-1   4d6kh3   4d6dl1   adv   dis   4d6 1d8 d4
 */
#ifndef DICE_EXPRESSION_H
#define DICE_EXPRESSION_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dice {

constexpr int MostPerTerm  = 100;
constexpr int MostDice     = 200;
constexpr int MostSides    = 1000;
constexpr int MostModifier = 99999;

// Which of a group's dice count: all, the highest or lowest few, or all but them.
enum class Keep : std::uint8_t { All, Highest, Lowest, DropHighest, DropLowest };

struct Term {
    bool        Negative = false;
    int         Count    = 0;       // dice in the group; 0 for a plain number
    int         Sides    = 0;
    Keep        Rule     = Keep::All;
    int         Amount   = 0;       // the few Rule keeps or drops
    int         Modifier = 0;       // the plain number
    std::string Spelled;            // as written, in lower case: 4d6kh3, adv, 3
};

struct Expression {
    std::vector<Term> Terms;

    int DiceCount() const;
};

struct ParseError {
    std::string Message;
};

std::expected<Expression, ParseError> Parse(std::string_view Text);

// The expression written out again: 4d6kh3+2.
std::string Spell(const Expression &Of);

// What the grammar is, a line at a time, for help and for the errors to point at.
std::span<const std::string_view> GrammarLines();

}  // namespace dice

#endif
