/* growth.h — what a plant's record makes of it: how far it has grown from how
 * often its program was started, how fresh it is from how lately, and the
 * ranking of them all. Numbers and words; nothing of the screen. */
#ifndef GARDEN_GROWTH_H
#define GARDEN_GROWTH_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "look.h"
#include "record.h"

namespace garden {

enum class Stage : std::uint8_t { Sprout, Seedling, Young, Blooming, Grown, Tree };

struct Growth {
    Stage  Reached = Stage::Sprout;
    double Height = 0;       // in pixels, as tall as the starts have made it
    double Fresh = 1;        // 1 for a program used today, 0 for one unused for the wilt days
};

// The height of a plant started 128 times: the tallest, and what the garden's room is fitted to.
constexpr double TallestHeight = 55.0;

Growth GrowthOf(const Plant &One, std::uint64_t Now, const Look &Shown);

const char *StageName(Stage Reached) noexcept;
const char *OrderName(Order Ranking) noexcept;
// How the plant fares: thriving, fine, thirsty, wilting, dormant.
const char *StateName(double Fresh) noexcept;
// "just now", "5 minutes ago", "3 days ago".
std::string Ago(std::uint64_t Seconds);
// The same in two or three characters: "now", "5m", "3d".
std::string Brief(std::uint64_t Seconds);

// The plants that count, ranked as the person set, no more than they asked.
std::vector<const Plant *> Ranked(const Record &Kept, const Look &Shown);

// A number scattered over all of 32 bits, the same every time; for the
// places a plant's leaves and a night's stars fall.
std::uint32_t Scatter(std::uint32_t Value) noexcept;
std::uint32_t Seed(std::string_view Name) noexcept;
// A scattered number as a fraction of one.
double Unit(std::uint32_t Value) noexcept;

}  // namespace garden

#endif
