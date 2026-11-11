/* history.cpp — the load over time. */

#include "history.h"

namespace taskboard {

namespace {

// The load between two counts, none when the total did not move.
std::optional<float> LoadBetween(std::uint64_t BusyBefore, std::uint64_t TotalBefore, std::uint64_t BusyAfter,
                                 std::uint64_t TotalAfter)
{
    if (TotalAfter <= TotalBefore) return std::nullopt;
    if (BusyAfter < BusyBefore) return 0.0f;
    const double Load = static_cast<double>(BusyAfter - BusyBefore) / static_cast<double>(TotalAfter - TotalBefore);
    return static_cast<float>(Load > 1 ? 1 : Load);
}

}  // namespace

void History::Take(const Reading &Now, std::uint32_t KeepSeconds)
{
    if (Before.size() == Now.Cores.size() && !Now.Cores.empty()) {
        Sample        One{.AtUs = Now.AtUs, .Machine = 0, .Cores = {}};
        std::uint64_t BusyBefore = 0, TotalBefore = 0, BusyAfter = 0, TotalAfter = 0;
        One.Cores.reserve(Now.Cores.size());
        for (std::size_t Index = 0; Index < Now.Cores.size(); Index++) {
            const CoreSeen &Was = Before[Index], &Is = Now.Cores[Index];
            const std::optional<float> Load = LoadBetween(Was.Busy, Was.Total, Is.Busy, Is.Total);
            One.Cores.push_back(Load);
            if (!Load) continue;   // the machine's load is that of the cores counted
            BusyBefore += Was.Busy;
            TotalBefore += Was.Total;
            BusyAfter += Is.Busy;
            TotalAfter += Is.Total;
        }
        One.Machine = LoadBetween(BusyBefore, TotalBefore, BusyAfter, TotalAfter).value_or(0.0f);
        Samples.push_back(std::move(One));
    } else {
        // The cores are not those of the reading before: what was kept of
        // them speaks of other cores.
        Samples.clear();
    }
    Before = Now.Cores;
    const std::uint64_t Keep = static_cast<std::uint64_t>(KeepSeconds) * 1000000u;
    while (!Samples.empty() && Now.AtUs - Samples.front().AtUs > Keep) Samples.pop_front();
}

double History::Machine() const noexcept
{
    return Samples.empty() ? 0 : Samples.back().Machine;
}

std::optional<double> History::Core(std::size_t Index) const noexcept
{
    if (Samples.empty() || Index >= Samples.back().Cores.size() || !Samples.back().Cores[Index]) return std::nullopt;
    return *Samples.back().Cores[Index];
}

std::vector<std::optional<float>> History::Columns(std::optional<std::size_t> Core, std::size_t Count,
                                                   std::uint32_t Seconds) const
{
    std::vector<std::optional<float>> Out(Count);
    if (Samples.empty() || Count == 0) return Out;
    const auto Value = [&](const Sample &One) -> std::optional<float> {
        if (!Core) return One.Machine;
        return *Core < One.Cores.size() ? One.Cores[*Core] : std::nullopt;
    };
    const std::uint64_t End  = Samples.back().AtUs;
    const std::uint64_t Span = static_cast<std::uint64_t>(Seconds) * 1000000u / Count + 1;
    std::size_t         At   = 0;
    std::optional<float> Held;
    for (std::size_t Column = 0; Column < Count; Column++) {
        const std::uint64_t Back  = static_cast<std::uint64_t>(Count - Column) * Span;
        const std::uint64_t From  = End >= Back ? End - Back : 0;
        const std::uint64_t Until = From + Span;
        // Readings before this span only hold what the graph shows until one
        // comes: the latest of them is what this span holds if it saw none.
        while (At < Samples.size() && Samples[At].AtUs < From) Held = Value(Samples[At++]);
        double      Sum  = 0;
        std::size_t Seen = 0;
        for (; At < Samples.size() && Samples[At].AtUs < Until; At++)
            if (const std::optional<float> One = Value(Samples[At])) {
                Sum += *One;
                Seen++;
                Held = One;
            }
        if (Seen) Out[Column] = static_cast<float>(Sum / static_cast<double>(Seen));
        else if (Held) Out[Column] = Held;
    }
    return Out;
}

}  // namespace taskboard
