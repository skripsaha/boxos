#ifndef TYPECASE_GLYPH_CACHE_H
#define TYPECASE_GLYPH_CACHE_H

/* Glyphs drawn, kept for when they are asked again — a screen shows the
 * same few hundred characters over and over, at a few sizes. One cache
 * keeps them all, asked two ways: a character in a cell, drawn by a chain
 * of sources; and a font's glyph for text, by its look (glyph_look.h), or
 * the box of a character no font has. What is kept stays within a budget of
 * bytes its owner sets; past it, the picture asked longest ago goes first,
 * and the one just asked always stays. One cache is asked by one strand at
 * a time. */

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "typecase/chain.h"
#include "typecase/font.h"
#include "typecase/glyph_look.h"
#include "typecase/rasterizer.h"
#include "typecase/size.h"

namespace typecase {

class GlyphCache {
public:
    explicit GlyphCache(std::size_t Budget);

    /* The character drawn by From in a cell of the size. */
    const GlyphImage &Get(Chain &From, char32_t Character, const Size &At);
    /* The font's glyph drawn for text as Look draws it. */
    const GlyphImage &Get(const Font &Face, GlyphId Glyph, const GlyphLook &Look);
    /* A character no font has, drawn for text as its code in a box of Cell
     * (DrawCodeBox). */
    const GlyphImage &GetMissing(char32_t Character, const Size &Cell);

    /* Each picture holds until the next Get or Clear. */
    void        Clear() noexcept;
    std::size_t Held() const noexcept { return Bytes; }   /* the bytes kept */

private:
    /* What drew a picture — a chain or a font by its serial, or none for a
     * missing character's box — what of it, and how. */
    struct Key {
        std::uint64_t Source   = 0;
        std::uint32_t Code     = 0;
        float         Em       = 0;
        std::int32_t  Width    = 0;
        std::int32_t  Height   = 0;
        std::int32_t  Baseline = 0;
        std::uint8_t  Look     = 0;

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
    const GlyphImage  *Find(const Key &Of);
    const GlyphImage  &Keep(const Key &Of, GlyphImage Image);

    std::size_t                                                 Budget;
    std::size_t                                                 Bytes = 0;
    std::list<Kept>                                             Order;   /* the most recently asked first */
    std::unordered_map<Key, std::list<Kept>::iterator, KeyHash> Index;
    Rasterizer                                                  Pen;
};

}  // namespace typecase

#endif  // TYPECASE_GLYPH_CACHE_H
