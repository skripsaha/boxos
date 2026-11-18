/* glyph_cache.cpp — glyphs drawn, kept within a budget. */

#include "typecase/glyph_cache.h"

#include <bit>
#include <utility>

#include "typecase/code_box.h"

namespace typecase {

GlyphCache::GlyphCache(std::size_t Budget) : Budget(Budget) {}

std::size_t GlyphCache::KeyHash::operator()(const Key &Of) const noexcept
{
    std::uint64_t Mix = Of.Source * 0x9E3779B97F4A7C15ull ^ Of.Code;
    for (std::uint32_t Part : {std::bit_cast<std::uint32_t>(Of.Em), static_cast<std::uint32_t>(Of.Width),
                               static_cast<std::uint32_t>(Of.Height), static_cast<std::uint32_t>(Of.Baseline),
                               static_cast<std::uint32_t>(Of.Look)})
        Mix = (Mix ^ Part) * 0x100000001B3ull;
    return static_cast<std::size_t>(Mix ^ (Mix >> 29));
}

/* What a kept glyph costs: its picture, and the bookkeeping round it. */
std::size_t GlyphCache::CostOf(const GlyphImage &Image) noexcept
{
    return Image.Coverage.size() + sizeof(Kept) + 4 * sizeof(void *);
}

const GlyphImage *GlyphCache::Find(const Key &Of)
{
    const auto Found = Index.find(Of);
    if (Found == Index.end()) return nullptr;
    Order.splice(Order.begin(), Order, Found->second);
    return &Found->second->Image;
}

const GlyphImage &GlyphCache::Keep(const Key &Of, GlyphImage Image)
{
    Order.push_front(Kept{Of, std::move(Image)});
    Index.emplace(Of, Order.begin());
    Bytes += CostOf(Order.front().Image);
    while (Bytes > Budget && Order.size() > 1) {
        Bytes -= CostOf(Order.back().Image);
        Index.erase(Order.back().Of);
        Order.pop_back();
    }
    return Order.front().Image;
}

const GlyphImage &GlyphCache::Get(Chain &From, char32_t Character, const Size &At)
{
    const Key Of{From.Serial(), Character, At.Em, At.Width, At.Height, At.Baseline, 0};
    if (const GlyphImage *Found = Find(Of)) return *Found;
    return Keep(Of, From.Draw(Character, At));
}

const GlyphImage &GlyphCache::Get(const Font &Face, GlyphId Glyph, const GlyphLook &Look)
{
    const auto Flags = static_cast<std::uint8_t>((Look.Phase & 3u) | (Look.Bold ? 4u : 0u) | (Look.Slant ? 8u : 0u));
    const Key  Of{Face.Serial(), Glyph, Look.Em, 0, 0, 0, Flags};
    if (const GlyphImage *Found = Find(Of)) return *Found;
    return Keep(Of, DrawGlyph(Face, Glyph, Look, Pen));
}

const GlyphImage &GlyphCache::GetMissing(char32_t Character, const Size &Cell)
{
    const Key Of{0, Character, Cell.Em, Cell.Width, Cell.Height, Cell.Baseline, 0};
    if (const GlyphImage *Found = Find(Of)) return *Found;
    return Keep(Of, DrawCodeBox(Character, Cell));
}

void GlyphCache::Clear() noexcept
{
    Index.clear();
    Order.clear();
    Bytes = 0;
}

}  // namespace typecase
