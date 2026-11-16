/* pages.cpp — the pages glyphs turns through. See pages.h. */
#include "pages.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "blocks.h"
#include "boxos_symbols.h"

namespace glyphs {

namespace {

std::vector<Page> Made()
{
    std::vector<Page> All;
    for (const UnicodeBlock &Each : UnicodeBlocks) All.push_back(Page{Each.First, Each.Last, Each.Name, false});
    char32_t First = BoxSymbols[0].Character, Last = First;
    for (const BoxSymbol &Each : BoxSymbols) {
        First = std::min<char32_t>(First, Each.Character);
        Last  = std::max<char32_t>(Last, Each.Character);
    }
    All.push_back(Page{First, Last, "BoxOS", true});
    return All;
}

/* The code a word spells — four to six hexadecimal digits, or any number of
 * them after U+ — when it is a character's. */
std::optional<char32_t> CodeOf(std::string_view Word)
{
    const bool Marked = Word.size() > 2 && (Word[0] == 'U' || Word[0] == 'u') && Word[1] == '+';
    if (Marked) Word.remove_prefix(2);
    if (Word.empty() || Word.size() > 6 || (!Marked && Word.size() < 4)) return std::nullopt;
    std::uint32_t Code = 0;
    for (const char Digit : Word) {
        const int Value = Digit >= '0' && Digit <= '9' ? Digit - '0'
                        : Digit >= 'a' && Digit <= 'f' ? Digit - 'a' + 10
                        : Digit >= 'A' && Digit <= 'F' ? Digit - 'A' + 10
                                                       : -1;
        if (Value < 0) return std::nullopt;
        Code = Code * 16 + static_cast<std::uint32_t>(Value);
    }
    if (Code > 0x10FFFF) return std::nullopt;
    return static_cast<char32_t>(Code);
}

char Lower(char Letter)
{
    return Letter >= 'A' && Letter <= 'Z' ? static_cast<char>(Letter - 'A' + 'a') : Letter;
}

bool Holds(std::string_view Name, std::string_view Text)
{
    return std::search(Name.begin(), Name.end(), Text.begin(), Text.end(),
                       [](char A, char B) { return Lower(A) == Lower(B); }) != Name.end();
}

}  // namespace

std::span<const Page> Pages()
{
    static const std::vector<Page> All = Made();
    return All;
}

std::optional<std::size_t> PageOf(char32_t Character)
{
    const std::span<const Page> All = Pages();
    if (!OwnName(Character).empty()) return All.size() - 1;
    for (std::size_t Index = 0; Index < All.size(); Index++)
        if (Character >= All[Index].First && Character <= All[Index].Last) return Index;
    return std::nullopt;
}

std::optional<Place> Find(std::string_view Typed, std::size_t After)
{
    if (Typed.empty()) return std::nullopt;
    if (const std::optional<char32_t> Code = CodeOf(Typed)) {
        if (const std::optional<std::size_t> On = PageOf(*Code)) return Place{*On, *Code};
        return std::nullopt;
    }
    const std::span<const Page> All = Pages();
    for (std::size_t Step = 1; Step <= All.size(); Step++) {
        const std::size_t Index = (After + Step) % All.size();
        if (Holds(All[Index].Name, Typed)) return Place{Index, All[Index].First};
    }
    return std::nullopt;
}

std::string_view OwnName(char32_t Character)
{
    for (const BoxSymbol &Each : BoxSymbols)
        if (Each.Character == Character) return Each.Name;
    return {};
}

}  // namespace glyphs
