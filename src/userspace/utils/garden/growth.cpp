/* growth.cpp — growth, freshness and the ranking. */

#include "growth.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace garden {

namespace {

constexpr std::uint64_t DaySeconds = 86400;

Stage StageOf(std::uint32_t Starts) noexcept
{
    if (Starts >= 60) return Stage::Tree;
    if (Starts >= 25) return Stage::Grown;
    if (Starts >= 10) return Stage::Blooming;
    if (Starts >= 4) return Stage::Young;
    if (Starts >= 2) return Stage::Seedling;
    return Stage::Sprout;
}

std::string Counted(std::uint64_t Many, std::string_view Unit)
{
    return std::format("{} {}{} ago", Many, Unit, Many == 1 ? "" : "s");
}

bool Before(const Plant *A, const Plant *B, Order Ranking)
{
    switch (Ranking) {
    case Order::Recent:
        if (A->Last != B->Last) return A->Last > B->Last;
        break;
    case Order::Name:
        if (A->Name != B->Name) return A->Name < B->Name;
        break;
    case Order::Starts:
        if (A->Starts != B->Starts) return A->Starts > B->Starts;
        if (A->Last != B->Last) return A->Last > B->Last;
        break;
    }
    return A->Name < B->Name;
}

}  // namespace

Growth GrowthOf(const Plant &One, std::uint64_t Now, const Look &Shown)
{
    const double Idle = Now > One.Last ? static_cast<double>(Now - One.Last) / static_cast<double>(DaySeconds) : 0.0;
    return Growth{.Reached = StageOf(One.Starts),
                  .Height  = std::min(TallestHeight, 6.0 + 7.0 * std::log2(static_cast<double>(std::max<std::uint32_t>(One.Starts, 1)))),
                  .Fresh   = std::clamp(1.0 - Idle / static_cast<double>(Shown.WiltDays), 0.0, 1.0)};
}

const char *StageName(Stage Reached) noexcept
{
    switch (Reached) {
    case Stage::Sprout:   return "sprout";
    case Stage::Seedling: return "seedling";
    case Stage::Young:    return "young plant";
    case Stage::Blooming: return "flowering plant";
    case Stage::Grown:    return "grown plant";
    case Stage::Tree:     break;
    }
    return "tree";
}

const char *OrderName(Order Ranking) noexcept
{
    return Ranking == Order::Recent ? "how lately" : Ranking == Order::Name ? "name" : "starts";
}

const char *StateName(double Fresh) noexcept
{
    if (Fresh >= 0.75) return "thriving";
    if (Fresh >= 0.5) return "fine";
    if (Fresh >= 0.25) return "thirsty";
    if (Fresh >= 0.08) return "wilting";
    return "dormant";
}

std::string Ago(std::uint64_t Seconds)
{
    if (Seconds < 60) return "just now";
    if (Seconds < 3600) return Counted(Seconds / 60, "minute");
    if (Seconds < DaySeconds) return Counted(Seconds / 3600, "hour");
    if (Seconds < 14 * DaySeconds) return Counted(Seconds / DaySeconds, "day");
    if (Seconds < 60 * DaySeconds) return Counted(Seconds / (7 * DaySeconds), "week");
    if (Seconds < 730 * DaySeconds) return Counted(Seconds / (30 * DaySeconds), "month");
    return Counted(Seconds / (365 * DaySeconds), "year");
}

std::string Brief(std::uint64_t Seconds)
{
    if (Seconds < 60) return "now";
    if (Seconds < 3600) return std::format("{}m", Seconds / 60);
    if (Seconds < DaySeconds) return std::format("{}h", Seconds / 3600);
    if (Seconds < 14 * DaySeconds) return std::format("{}d", Seconds / DaySeconds);
    if (Seconds < 60 * DaySeconds) return std::format("{}w", Seconds / (7 * DaySeconds));
    if (Seconds < 730 * DaySeconds) return std::format("{}mo", Seconds / (30 * DaySeconds));
    return std::format("{}y", Seconds / (365 * DaySeconds));
}

std::vector<const Plant *> Ranked(const Record &Kept, const Look &Shown)
{
    std::vector<const Plant *> List;
    for (const Plant &One : Kept.Plants) {
        if (One.System && !Shown.System) continue;
        if ((One.Name == "garden" || One.Name == "gardener") && !Shown.Itself) continue;
        List.push_back(&One);
    }
    std::sort(List.begin(), List.end(), [&](const Plant *A, const Plant *B) { return Before(A, B, Shown.Ranking); });
    if (List.size() > Shown.Shown) List.resize(Shown.Shown);
    return List;
}

std::uint32_t Scatter(std::uint32_t Value) noexcept
{
    Value ^= Value >> 16;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15;
    Value *= 0x846CA68Bu;
    Value ^= Value >> 16;
    return Value;
}

std::uint32_t Seed(std::string_view Name) noexcept
{
    std::uint32_t Hash = 2166136261u;
    for (const char C : Name) Hash = (Hash ^ static_cast<unsigned char>(C)) * 16777619u;
    return Scatter(Hash);
}

double Unit(std::uint32_t Value) noexcept
{
    return static_cast<double>(Value >> 8) / 16777216.0;
}

}  // namespace garden
