/* text.h — the few things the hub asks of a piece of typed text. */
#ifndef HUB_TEXT_H
#define HUB_TEXT_H

#include <string_view>

namespace hub::text {

// A blank separates words: a space or a tab, as the Luggage and the Form cut.
constexpr bool Blank(char C) noexcept
{
    return C == ' ' || C == '\t';
}

// Text with the blanks at both ends left off.
std::string_view Trimmed(std::string_view Text) noexcept;

// Typed says Word, in either case: Word is written in small letters.
bool Says(std::string_view Typed, std::string_view Word) noexcept;

}  // namespace hub::text

#endif
