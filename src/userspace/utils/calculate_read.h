#ifndef CALCULATE_READ_H
#define CALCULATE_READ_H

/* A typed sum cut into tokens, each with the place in the text it stands. */

#include "calculate.h"

struct Span {
    std::size_t Begin;
    std::size_t End;
};

enum class Kind { Number, Name, Symbol, End };

struct Token {
    Kind        Type   = Kind::End;
    Span        At     = {0, 0};
    char        Symbol = 0;
    std::string Name;
    Number      Value;
};

inline char Lowered(char Ch)
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

/* The last token is always End, standing just after the text. Throws
 * CalculateError for a character or a number that cannot be read. */
std::vector<Token> Tokenize(std::string_view Text);

#endif
