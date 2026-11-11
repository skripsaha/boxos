/* rules.h — what a game is played by: the grid, the fleet, and the three
 * rules the players may change. */
#ifndef SEABATTLE_RULES_H
#define SEABATTLE_RULES_H

#include <span>
#include <string>
#include <string_view>

namespace seabattle {

enum class FleetKind { Classic, Skirmish, Armada, Duel };

inline constexpr int FleetKindCount = 4;
inline constexpr int LongestShip = 5;

struct Rules {
    int       Grid = 10;
    FleetKind Fleet = FleetKind::Classic;
    bool      Touching = false;     // ships may lie side by side, corner to corner
    bool      ExtraShot = true;     // a hit earns another shot
    bool      Salvo = false;        // as many shots in a turn as ships afloat
};

// The lengths of a fleet's ships, longest first.
std::span<const int> FleetLengths(FleetKind Kind);
std::string_view     FleetName(FleetKind Kind);
// "1x4 2x3 3x2 4x1": how many ships of each length, longest first.
std::string          FleetSummary(FleetKind Kind);
int                  FleetCells(FleetKind Kind);
// What a ship of this many cells is called.
std::string_view     ShipName(int Length);

}  // namespace seabattle

#endif
