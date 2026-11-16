/* imprint_make.h — the imprint (boxos_imprint.h) made from the system's font:
 * the characters the system says, drawn by the chain a screen of cells asks —
 * BoxOS's own symbols, geometry, the font — at each cell size, and written as
 * the header the hands that read it include. tools/imprint makes it at every
 * build; the typecase stand holds it. */
#ifndef TOOLS_IMPRINT_MAKE_H
#define TOOLS_IMPRINT_MAKE_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "boxos_imprint.h"

/* The heights of the cells an imprint is made at; each cell as wide as the
 * font's proportions make it. */
inline constexpr std::int32_t ImprintHeights[] = {16, 24, 32};

struct MadeImprint {
    std::vector<std::uint32_t>  Characters;
    std::vector<BoxImprintSize> Sizes;
    std::vector<std::uint8_t>   Pixels;

    BoxImprint View() const
    {
        return BoxImprint{Characters.data(), static_cast<std::uint32_t>(Characters.size()), Sizes.data(),
                          static_cast<std::uint32_t>(Sizes.size()), Pixels.data()};
    }
};

/* What the imprint is asked to hold — what the system says: Latin, Greek and
 * Cyrillic, punctuation, arrows and a few signs, frames, blocks and shapes,
 * the replacement character, Duppo, and every character the text page folds
 * to a glyph (boxos_glyph_fold.h). A character the chain cannot draw is left
 * out, and drawn as the empty box. In order of code, each once. */
std::vector<char32_t> ImprintWanted();

/* The imprint of the font Bytes are; why not, when they are no font. */
std::expected<MadeImprint, std::string> MakeImprint(std::vector<std::uint8_t> Bytes);

/* The imprint as the header boxos_imprint_data.h, made from the font called
 * FontName: the system's, BoxSystemImprint — or, Called Bold, BoxBoldImprint
 * and arrays of its own, for a hand that carries both. */
std::string ImprintHeader(const MadeImprint &Made, const std::string &FontName, const std::string &Called);

#endif  // TOOLS_IMPRINT_MAKE_H
