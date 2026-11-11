/* materials.h — what the sandbox is made of, and how each thing behaves in
 * the plainest terms: how it moves, how heavy it is, whether it burns. */
#ifndef SAND_MATERIALS_H
#define SAND_MATERIALS_H

#include <array>
#include <cstdint>

namespace sand {

enum class Kind : std::uint8_t { Air, Sand, Water, Oil, Stone, Wood, Plant, Seed, Fire, Smoke, Steam, Lava, Glass, Count };

inline constexpr int KindCount = static_cast<int>(Kind::Count);

enum class Habit : std::uint8_t { Empty, Powder, Liquid, Gas, Flame, Solid };

struct Trait {
    const char *Name;
    Habit       Moves;
    int         Density;   // heavier sinks, below zero rises
    int         Ignites;   // one chance in so many a step beside fire; 0 does not burn
    int         Fuel;      // steps it burns for
};

const Trait &TraitOf(Kind Which);

// What can be poured, in the order of the number keys.
inline constexpr std::array<Kind, 10> Pourable{Kind::Sand, Kind::Water, Kind::Stone, Kind::Wood, Kind::Oil,
                                               Kind::Fire, Kind::Lava,  Kind::Seed,  Kind::Plant, Kind::Glass};

// The eraser stands after the last material in the palette.
inline constexpr int EraserAt = static_cast<int>(Pourable.size());

}  // namespace sand

#endif
