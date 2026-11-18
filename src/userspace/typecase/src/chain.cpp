/* chain.cpp — sources asked in turn for a character. */

#include "typecase/chain.h"

#include <atomic>
#include <utility>

#include "typecase/code_box.h"

namespace typecase {

namespace {

std::atomic<std::uint64_t> NextSerial{1};

}  // namespace

Chain::Chain() : Number(NextSerial.fetch_add(1, std::memory_order_relaxed)) {}

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
