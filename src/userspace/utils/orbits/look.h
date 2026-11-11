// look.h — what the person set of the sky (settings orbits): read whole, and
// read again when the file is saved or the Use Context changes.
#ifndef ORBITS_LOOK_H
#define ORBITS_LOOK_H

#include <cstddef>
#include <cstdint>

#include "box/types.h"
#include "key_chord.h"
#include "painter/core/color.h"

namespace orbits {

using Color = painter::core::Color;

// The colours the bodies take, by their tone.
enum class Palette { Spectrum, Ember, Ocean, Aurora, Mono };

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, w a s d, Enter, Space, Esc and the plain keys, which no setting takes.
enum OrbitKey : std::uint8_t { KeyHelp, KeyClear, KeyReset, KeyAim, KeyCount };

struct Look {
    std::size_t   Preset;           // the sky it begins with, as presets.h counts
    std::uint32_t TrailSeconds;     // how long a trail takes to fade
    std::uint32_t GravityPercent;
    std::uint32_t SpeedPercent;     // how fast time runs when it begins
    Palette       Colors;
    Color         Star;
    Color         Background;
    bool          ShowAim;
    Color         Aim;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

// The colour of a body of Tone 0..1, and of a star, which is the star colour
// tinted by its tone.
Color BodyColor(const Look &Shown, double Tone);
Color StarColor(const Look &Shown, double Tone);

}  // namespace orbits

#endif  // ORBITS_LOOK_H
