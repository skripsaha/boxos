#ifndef TYPECASE_CHAIN_H
#define TYPECASE_CHAIN_H

/* Sources asked in turn for a character: the first that draws it answers,
 * and a character none draws is drawn as its code (DrawCodeBox), so a chain
 * always answers. Whoever builds a chain chooses its sources and their
 * order — a screen of cells puts the sources that draw to the cell first,
 * so a line of a frame meets the next cell's exactly, and its fonts after;
 * running text needs only fonts. */

#include <memory>
#include <vector>

#include "typecase/glyph_source.h"

namespace typecase {

class Chain {
public:
    /* Source, asked after every source added before it. */
    void Add(std::unique_ptr<GlyphSource> Source);

    /* Whether some source has the character: whether it is shown as itself. */
    bool       Has(char32_t Character) const;
    GlyphImage Draw(char32_t Character, const Size &At);

private:
    std::vector<std::unique_ptr<GlyphSource>> Sources;
};

}  // namespace typecase

#endif  // TYPECASE_CHAIN_H
