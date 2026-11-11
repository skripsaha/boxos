// presets.h — the skies it can begin with, each set up in the sky's own units
// (sky.h) at the gravity the sky has now, so that its orbits are the orbits it
// was made for.
#ifndef ORBITS_PRESETS_H
#define ORBITS_PRESETS_H

#include <cstddef>
#include <cstdint>

#include "sky.h"

namespace orbits {

inline constexpr std::size_t PresetCount = 5;

// The preset's name, as the form and the settings say it.
const char *PresetName(std::size_t Which);

// Its place in the list; PresetCount when there is none of the name.
std::size_t PresetNamed(const char *Name);

// The sky made over as the preset; Seed varies what it leaves to chance.
void Prepare(Sky &Sky, std::size_t Which, std::uint64_t Seed);

}  // namespace orbits

#endif  // ORBITS_PRESETS_H
