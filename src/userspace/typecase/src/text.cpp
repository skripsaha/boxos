/* text.cpp — a text to be set, and UTF-8 both ways. */

#include "typecase/text.h"

#include <utility>

namespace typecase {

std::string Utf8Of(std::u32string_view Characters)
{
    std::string Said;
    Said.reserve(Characters.size());
    for (char32_t Character : Characters) {
        if (Character > 0x10FFFF || (Character >= 0xD800 && Character < 0xE000)) Character = 0xFFFD;
        if (Character < 0x80) {
            Said += static_cast<char>(Character);
        } else if (Character < 0x800) {
            Said += static_cast<char>(0xC0 | Character >> 6);
            Said += static_cast<char>(0x80 | (Character & 0x3F));
        } else if (Character < 0x10000) {
            Said += static_cast<char>(0xE0 | Character >> 12);
            Said += static_cast<char>(0x80 | (Character >> 6 & 0x3F));
            Said += static_cast<char>(0x80 | (Character & 0x3F));
        } else {
            Said += static_cast<char>(0xF0 | Character >> 18);
            Said += static_cast<char>(0x80 | (Character >> 12 & 0x3F));
            Said += static_cast<char>(0x80 | (Character >> 6 & 0x3F));
            Said += static_cast<char>(0x80 | (Character & 0x3F));
        }
    }
    return Said;
}

/* A character's bytes as Unicode asks them read: the shortest form only, no
 * surrogate, nothing past U+10FFFF; what is not that is U+FFFD, and reading
 * goes on at the first byte that could not continue it. */
std::u32string CharactersOf(std::string_view Utf8)
{
    std::u32string Said;
    Said.reserve(Utf8.size());
    std::size_t At = 0;
    while (At < Utf8.size()) {
        const auto  Lead   = static_cast<std::uint8_t>(Utf8[At]);
        std::size_t Length = Lead < 0x80 ? 1 : Lead >= 0xC2 && Lead < 0xE0 ? 2 : Lead >= 0xE0 && Lead < 0xF0 ? 3
                                       : Lead >= 0xF0 && Lead < 0xF5 ? 4 : 0;
        if (Length == 0) {
            Said += U'�';
            At += 1;
            continue;
        }
        char32_t Character = Length == 1 ? Lead : Lead & (0x7F >> Length);
        std::size_t Read = 1;
        for (; Read < Length && At + Read < Utf8.size(); Read++) {
            const auto Next = static_cast<std::uint8_t>(Utf8[At + Read]);
            /* The second byte's range closes the overlong and surrogate forms. */
            const std::uint8_t Low  = Read == 1 && Lead == 0xE0 ? 0xA0 : Read == 1 && Lead == 0xF0 ? 0x90 : 0x80;
            const std::uint8_t High = Read == 1 && Lead == 0xED ? 0x9F : Read == 1 && Lead == 0xF4 ? 0x8F : 0xBF;
            if (Next < Low || Next > High) break;
            Character = Character << 6 | (Next & 0x3F);
        }
        if (Read < Length) {
            Said += U'�';
            At += Read;
            continue;
        }
        Said += Character;
        At += Length;
    }
    return Said;
}

Text Text::Of(std::string_view Utf8, typecase::Style Style)
{
    Text Made;
    Made.Characters = CharactersOf(Utf8);
    Made.Style      = std::move(Style);
    return Made;
}

}  // namespace typecase
