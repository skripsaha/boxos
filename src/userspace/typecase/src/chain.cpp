/* chain.cpp — sources asked in turn for a character. */

#include "typecase/chain.h"

#include <utility>

#include "typecase/code_box.h"

namespace typecase {

void Chain::Add(std::unique_ptr<GlyphSource> Source)
{
    if (Source) Sources.push_back(std::move(Source));
}

bool Chain::Has(char32_t Character) const
{
    for (const auto &Source : Sources)
        if (Source->Has(Character)) return true;
    return false;
}

GlyphImage Chain::Draw(char32_t Character, const Size &At)
{
    for (const auto &Source : Sources)
        if (auto Image = Source->Draw(Character, At)) return std::move(*Image);
    return DrawCodeBox(Character, At);
}

}  // namespace typecase
