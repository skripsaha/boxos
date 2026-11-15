/* glyph_cache.cpp — glyphs a chain has drawn, kept within a budget. */

#include "typecase/glyph_cache.h"

#include <bit>

namespace typecase {

GlyphCache::GlyphCache(Chain &From, std::size_t Budget) : From(From), Budget(Budget) {}

std::size_t GlyphCache::KeyHash::operator()(const Key &Of) const noexcept
{
    std::uint64_t Mix = Of.Character;
    for (std::uint32_t Part : {std::bit_cast<std::uint32_t>(Of.At.Em), static_cast<std::uint32_t>(Of.At.Width),
                               static_cast<std::uint32_t>(Of.At.Height), static_cast<std::uint32_t>(Of.At.Baseline)})
        Mix = (Mix ^ Part) * 0x100000001B3ull;
    return static_cast<std::size_t>(Mix ^ (Mix >> 29));
}

/* What a kept glyph costs: its picture, and the bookkeeping round it. */
std::size_t GlyphCache::CostOf(const GlyphImage &Image) noexcept
{
    return Image.Coverage.size() + sizeof(Kept) + 4 * sizeof(void *);
}

const GlyphImage &GlyphCache::Get(char32_t Character, const Size &At)
{
    const Key Of{Character, At};
    if (auto Found = Index.find(Of); Found != Index.end()) {
        Order.splice(Order.begin(), Order, Found->second);
        return Found->second->Image;
    }

    Order.push_front(Kept{Of, From.Draw(Character, At)});
    Index.emplace(Of, Order.begin());
    Bytes += CostOf(Order.front().Image);
    while (Bytes > Budget && Order.size() > 1) {
        Bytes -= CostOf(Order.back().Image);
        Index.erase(Order.back().Of);
        Order.pop_back();
    }
    return Order.front().Image;
}

void GlyphCache::Clear() noexcept
{
    Index.clear();
    Order.clear();
    Bytes = 0;
}

}  // namespace typecase
