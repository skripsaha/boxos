#ifndef TYPECASE_STAND_H
#define TYPECASE_STAND_H

/* typecase stand — what its parts share (tools/typecase_stand.cpp). */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "typecase/bitmap_source.h"

namespace stand {

/* Checks held and failed; a failure is said as it happens. */
struct Tally {
    int Held   = 0;
    int Failed = 0;

    void Check(bool Holds, const std::string &What);
};

std::vector<std::uint8_t> Load(const char *Path);

/* The bitmap font BoxOS carries (boxos_font.h) as a source: the characters
 * it folds to one glyph of its own (boxos_glyph_fold.h), as the display
 * draws them today. */
std::unique_ptr<typecase::BitmapSource> BoxFont();

/* Each part of the checks, adding to Count. */
void CheckRaster(Tally &Count);
void CheckForged(Tally &Count, const char *ForgeDir);
void CheckGlyphs(Tally &Count, const std::vector<std::uint8_t> &Font);
void CheckSources(Tally &Count, const std::vector<std::uint8_t> &Font);
void CheckGeometry(Tally &Count);
void CheckSymbols(Tally &Count);
void CheckDamage(Tally &Count, const std::vector<std::uint8_t> &Font, int Mutations);

}  // namespace stand

#endif  // TYPECASE_STAND_H
