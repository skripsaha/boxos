#ifndef TYPECASE_GLYPH_CACHE_H
#define TYPECASE_GLYPH_CACHE_H

/* Glyphs a chain has drawn, kept for when they are asked again — a screen
 * shows the same few hundred characters over and over. What is kept stays
 * within a budget of bytes its owner sets; past it, the glyph asked longest
 * ago goes first, and the one just asked always stays. */

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "typecase/chain.h"

namespace typecase {

class GlyphCache {
public:
    GlyphCache(Chain &From, std::size_t Budget);

    /* The character drawn at the size: kept from before, or drawn by the
     * chain now. The picture holds until the next Get or Clear. */
    const GlyphImage &Get(char32_t Character, const Size &At);

    void        Clear() noexcept;
    std::size_t Held() const noexcept { return Bytes; }   /* the bytes kept */

private:
    struct Key {
        char32_t Character;
        Size     At;

        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key &Of) const noexcept;
    };
    struct Kept {
        Key        Of;
        GlyphImage Image;
    };

    static std::size_t CostOf(const GlyphImage &Image) noexcept;

    Chain                                                      &From;
    std::size_t                                                 Budget;
    std::size_t                                                 Bytes = 0;
    std::list<Kept>                                             Order;   /* the most recently asked first */
    std::unordered_map<Key, std::list<Kept>::iterator, KeyHash> Index;
};

}  // namespace typecase

#endif  // TYPECASE_GLYPH_CACHE_H
