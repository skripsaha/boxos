/* materials.cpp — the table of what things are. */

#include "materials.h"

namespace sand {

namespace {

constexpr Trait Traits[KindCount] = {
    {"air",   Habit::Empty,  0,   0,  0},
    {"sand",  Habit::Powder, 15,  0,  0},
    {"water", Habit::Liquid, 10,  0,  0},
    {"oil",   Habit::Liquid, 8,   3,  140},
    {"stone", Habit::Solid,  100, 0,  0},
    {"wood",  Habit::Solid,  100, 45, 160},
    {"plant", Habit::Solid,  100, 14, 80},
    {"seed",  Habit::Powder, 12,  10, 30},
    {"fire",  Habit::Flame,  -1,  0,  0},
    {"smoke", Habit::Gas,    -2,  0,  0},
    {"steam", Habit::Gas,    -3,  0,  0},
    {"lava",  Habit::Liquid, 20,  0,  0},
    {"glass", Habit::Solid,  100, 0,  0},
};

}  // namespace

const Trait &TraitOf(Kind Which)
{
    return Traits[static_cast<int>(Which)];
}

}  // namespace sand
