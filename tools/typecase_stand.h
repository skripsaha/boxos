#ifndef TYPECASE_STAND_H
#define TYPECASE_STAND_H

/* typecase stand — what its parts share (tools/typecase_stand.cpp). */

#include <cstdint>
#include <string>
#include <vector>

#include "boxos_imprint.h"

namespace stand {

/* Checks held and failed; a failure is said as it happens. */
struct Tally {
    int Held   = 0;
    int Failed = 0;

    void Check(bool Holds, const std::string &What);
};

std::vector<std::uint8_t> Load(const char *Path);

/* Each part of the checks, adding to Count. */
void CheckRaster(Tally &Count);
void CheckForged(Tally &Count, const char *ForgeDir);
void CheckGlyphs(Tally &Count, const std::vector<std::uint8_t> &Font);
void CheckSources(Tally &Count, const std::vector<std::uint8_t> &Font);
void CheckGeometry(Tally &Count);
void CheckSymbols(Tally &Count);
void CheckImprint(Tally &Count, const std::vector<std::uint8_t> &Font);

/* The imprint's digest, one line: its count, each size's cell, offset and a
 * sum over every pixel of every glyph, and where three characters are. */
void SayImprintDigest(const BoxImprint &Of);
void CheckDamage(Tally &Count, const std::vector<std::uint8_t> &Font, int Mutations);

}  // namespace stand

#endif  // TYPECASE_STAND_H
