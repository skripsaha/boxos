/* paint_words.cpp — what the board's pictures say. See paint_words.h. */

#include "paint_words.h"

#include <format>

namespace taskboard {

Color LivingInk(const Look &Shown, Living Where)
{
    switch (Where) {
    case Living::OnScreen: return Shown.OnScreen;
    case Living::Behind:   return Shown.Behind;
    case Living::Keeper:   return Shown.Keeper;
    case Living::OnItsOwn: return Shown.OnItsOwn;
    case Living::Serving:  return Shown.Serving;
    case Living::Task:     break;
    }
    return Shown.Task;
}

Color HappenedInk(const Look &Shown, Happened What)
{
    switch (What) {
    case Happened::Began:  return Shown.Started;
    case Happened::Ended:  return Shown.Ended;
    case Happened::Fell:   return Shown.Fell;
    case Happened::Raised: return Shown.Raised;
    case Happened::Board:  break;
    }
    return Shown.Itself;
}

const char *StateName(std::uint32_t State)
{
    switch (State) {
    case 0: return "new";
    case 1: return "working";
    case 2: return "waiting";
    case 3: return "stopped";
    case 4: return "done";
    case 5: return "fell";
    default: return "?";
    }
}

std::string Bytes(std::uint64_t Count)
{
    constexpr std::uint64_t Kib = 1024, Mib = Kib * 1024, Gib = Mib * 1024;
    const auto Tenths = [](std::uint64_t N, std::uint64_t Unit) {
        const std::uint64_t T = N * 10 / Unit;
        return T >= 1000 ? std::format("{}", T / 10) : std::format("{}.{}", T / 10, T % 10);
    };
    if (Count >= Gib) return Tenths(Count, Gib) + "G";
    if (Count >= Mib) return Tenths(Count, Mib) + "M";
    if (Count >= Kib) return std::format("{}K", Count / Kib);
    return std::format("{}B", Count);
}

std::string Lasted(std::uint64_t Us)
{
    const std::uint64_t Seconds = Us / 1000000u, Minutes = Seconds / 60u, Hours = Minutes / 60u, Days = Hours / 24u;
    if (Seconds < 60) return std::format("{}s", Seconds);
    if (Minutes < 60) return std::format("{}m{:02}s", Minutes, Seconds % 60u);
    if (Hours < 24) return std::format("{}h{:02}m", Hours, Minutes % 60u);
    return std::format("{}d{:02}h", Days, Hours % 24u);
}

std::string Percent(double Share)
{
    return std::format("{}%", static_cast<long>(Share * 100.0 + 0.5));
}

const char *SortName(SortBy Sort)
{
    switch (Sort) {
    case SortBy::Memory:    return "by memory";
    case SortBy::Name:      return "by name";
    case SortBy::Age:       return "by age";
    case SortBy::Processor: break;
    }
    return "by processor";
}

}  // namespace taskboard
