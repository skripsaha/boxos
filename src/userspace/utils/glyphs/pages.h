/* pages.h — the pages glyphs turns through, and where what is typed leads. */
#ifndef GLYPHS_PAGES_H
#define GLYPHS_PAGES_H

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace glyphs {

/* A page of characters: a block of Unicode, shown in rows of sixteen, or
 * BoxOS's own characters (boxos_symbols.h), a list with their names. */
struct Page {
    char32_t         First, Last;
    std::string_view Name;
    bool             Own;
};

/* Every page: Unicode's blocks in order, then BoxOS's own. */
std::span<const Page> Pages();

/* The page Character stands on — BoxOS's own before the block of private
 * use that holds them; none for a code that no page has. */
std::optional<std::size_t> PageOf(char32_t Character);

/* Where what was typed leads: a code — 0416, U+0416, u+1fb3c — on its page;
 * or else the first page after After, round to it again, whose name holds
 * the text whatever the case of its letters, at its first character. */
struct Place {
    std::size_t Page;
    char32_t    Character;
};
std::optional<Place> Find(std::string_view Typed, std::size_t After);

/* The name of one of BoxOS's own characters; empty for any other. */
std::string_view OwnName(char32_t Character);

}  // namespace glyphs

#endif  // GLYPHS_PAGES_H
