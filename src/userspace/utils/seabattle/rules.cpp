/* rules.cpp — the fleets a game may be played with. */

#include "rules.h"

#include <array>
#include <format>
#include <numeric>

namespace seabattle {

namespace {

constexpr std::array<int, 10> Classic = {4, 3, 3, 2, 2, 2, 1, 1, 1, 1};
constexpr std::array<int, 5>  Skirmish = {3, 2, 2, 1, 1};
constexpr std::array<int, 11> Armada = {5, 4, 4, 3, 3, 2, 2, 2, 1, 1, 1};
constexpr std::array<int, 3>  Duel = {4, 3, 2};

}  // namespace

std::span<const int> FleetLengths(FleetKind Kind)
{
    switch (Kind) {
    case FleetKind::Skirmish: return Skirmish;
    case FleetKind::Armada:   return Armada;
    case FleetKind::Duel:     return Duel;
    case FleetKind::Classic:  break;
    }
    return Classic;
}

std::string_view FleetName(FleetKind Kind)
{
    switch (Kind) {
    case FleetKind::Skirmish: return "Skirmish";
    case FleetKind::Armada:   return "Armada";
    case FleetKind::Duel:     return "Duel";
    case FleetKind::Classic:  break;
    }
    return "Classic";
}

std::string FleetSummary(FleetKind Kind)
{
    std::array<int, LongestShip + 1> Count{};
    for (const int Length : FleetLengths(Kind)) Count[static_cast<std::size_t>(Length)]++;
    std::string Text;
    for (int Length = LongestShip; Length >= 1; Length--) {
        const int Many = Count[static_cast<std::size_t>(Length)];
        if (Many > 0) Text += std::format("{}{}x{}", Text.empty() ? "" : " ", Many, Length);
    }
    return Text;
}

std::string_view ShipName(int Length)
{
    switch (Length) {
    case 1:  return "Patrol boat";
    case 2:  return "Destroyer";
    case 3:  return "Cruiser";
    case 4:  return "Battleship";
    default: return "Carrier";
    }
}

int FleetCells(FleetKind Kind)
{
    const std::span<const int> Lengths = FleetLengths(Kind);
    return std::accumulate(Lengths.begin(), Lengths.end(), 0);
}

}  // namespace seabattle
