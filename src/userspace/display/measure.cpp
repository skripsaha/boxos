#include "measure.h"

#if defined(BOX_MEASURE)

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

#include "box/clock.h"
#include "box/debug.h"
#include "areas.h"

namespace {

constexpr std::size_t Parts = static_cast<std::size_t>(Part::Count);

constexpr const char *Names[Parts] = {"idle",    "other", "rotate", "intake",  "animate", "console", "take",
                                      "prepare", "compose", "kept", "straight", "turned", "faded",  "layered",
                                      "page",    "put",   "lay",    "flush"};

std::uint64_t Ticks() noexcept { return __builtin_ia32_rdtsc(); }

/* The display's time as it is spent: the part it is in now and since when,
 * and what each part had of this second. */
struct Account {
    Part          Now   = Part::Other;
    std::uint64_t Since = 0;
    std::uint64_t Spent[Parts]{};
    std::uint64_t Frames = 0, Composed = 0, Flushed = 0, Longest = 0;
    std::uint64_t SecondUs = 0, StartTicks = 0, StartUs = 0;
    /* The keys: when each waiting for a frame was said; of the second, how
     * many, and how long from being said to being taken and to the glass. */
    std::vector<std::uint64_t> Waiting;
    std::uint64_t              Keys = 0, Taken = 0, TakenMost = 0, Glassed = 0, GlassedMost = 0, GlassedCount = 0;
    /* The alarms of the second, and how late they came, in µs. */
    std::uint64_t Alarms = 0, Late = 0, LateMost = 0;
    double        TicksPerUs = 0;   // the counter's speed, once known
};
Account g;

void Credit() noexcept
{
    const std::uint64_t Now = Ticks();
    if (g.Since != 0) g.Spent[static_cast<std::size_t>(g.Now)] += Now - g.Since;
    g.Since = Now;
}

/* Hundredths, as "12.34". */
struct Fixed {
    char Text[24];
    explicit Fixed(double Of) noexcept
    {
        const auto Whole = static_cast<unsigned long long>(Of < 0 ? 0 : Of * 100.0 + 0.5);
        std::snprintf(Text, sizeof Text, "%llu.%02llu", Whole / 100, Whole % 100);
    }
};

void Say(const char *Line) noexcept { kdbg_print("[measure] %s", Line); }

}  // namespace

Clocked::Clocked(Part Into) noexcept : Outer(g.Now)
{
    Credit();
    g.Now = Into;
}

Clocked::~Clocked()
{
    Credit();
    g.Now = Outer;
}

std::uint64_t MeasureTicks() noexcept { return Ticks(); }

void MeasureFrame(std::uint64_t Began, std::uint64_t Pixels) noexcept
{
    g.Frames++;
    g.Composed += Pixels;
    g.Longest = std::max(g.Longest, Ticks() - Began);
}

void MeasureFlushed(std::uint64_t Pixels) noexcept { g.Flushed += Pixels; }

void MeasureKey(std::uint64_t Said) noexcept
{
    const std::uint64_t Now = Ticks();
    if (Said == 0 || Said > Now) return;
    g.Keys++;
    g.Taken += Now - Said;
    g.TakenMost = std::max(g.TakenMost, Now - Said);
    if (g.Waiting.size() < 64) g.Waiting.push_back(Said);
}

void MeasureAlarm(std::uint64_t Moment) noexcept
{
    const std::uint64_t Now = clock_uptime_us();
    const std::uint64_t By  = Now > Moment ? Now - Moment : 0;
    g.Alarms++;
    g.Late += By;
    g.LateMost = std::max(g.LateMost, By);
}

void MeasureGlassed() noexcept
{
    const std::uint64_t Now = Ticks();
    for (const std::uint64_t Said : g.Waiting) {
        g.Glassed += Now - Said;
        g.GlassedMost = std::max(g.GlassedMost, Now - Said);
        g.GlassedCount++;
    }
    g.Waiting.clear();
}

void MeasureSecond() noexcept
{
    const std::uint64_t Now = clock_uptime_us();
    if (g.StartUs == 0) {
        g.StartUs = g.SecondUs = Now;
        g.StartTicks = g.Since = Ticks();
        std::fill(std::begin(g.Spent), std::end(g.Spent), 0);
        return;
    }
    if (Now - g.SecondUs < 1000000) return;
    Credit();
    /* The counter's speed over everything clocked so far, once that is long
     * enough to be known: the seconds before it say nothing. */
    if (Now - g.StartUs >= 2000000) g.TicksPerUs = static_cast<double>(Ticks() - g.StartTicks) / static_cast<double>(Now - g.StartUs);
    const double PerMs = g.TicksPerUs * 1000.0;
    std::uint64_t Busy = 0;
    for (std::size_t P = 1; P < Parts; P++) Busy += g.Spent[P];
    /* A second of nothing to do says nothing. */
    if (g.TicksPerUs > 0 && (g.Frames != 0 || static_cast<double>(Busy) / PerMs > 20.0)) {
        char Line[256];
        std::snprintf(Line, sizeof Line, "%llu ms: %llu frames, longest %s ms; %llu px composed, %llu put on the glass; busy %s ms",
                      static_cast<unsigned long long>((Now - g.SecondUs) / 1000), static_cast<unsigned long long>(g.Frames),
                      Fixed(static_cast<double>(g.Longest) / PerMs).Text, static_cast<unsigned long long>(g.Composed),
                      static_cast<unsigned long long>(g.Flushed), Fixed(static_cast<double>(Busy) / PerMs).Text);
        Say(Line);
        for (std::size_t From = 0; From < Parts; From += 9) {
            int Length = std::snprintf(Line, sizeof Line, "ms");
            for (std::size_t P = From; P < std::min(Parts, From + 9); P++)
                Length += std::snprintf(Line + Length, sizeof Line - static_cast<std::size_t>(Length), " %s %s", Names[P],
                                        Fixed(static_cast<double>(g.Spent[P]) / PerMs).Text);
            Say(Line);
        }
        if (g.Keys != 0) {
            std::snprintf(Line, sizeof Line, "keys: %llu, taken %s ms after the kernel said them (most %s), on the glass after %s ms (most %s)",
                          static_cast<unsigned long long>(g.Keys), Fixed(static_cast<double>(g.Taken) / PerMs / static_cast<double>(g.Keys)).Text,
                          Fixed(static_cast<double>(g.TakenMost) / PerMs).Text,
                          Fixed(g.GlassedCount ? static_cast<double>(g.Glassed) / PerMs / static_cast<double>(g.GlassedCount) : 0).Text,
                          Fixed(static_cast<double>(g.GlassedMost) / PerMs).Text);
            Say(Line);
        }
        if (g.Alarms != 0) {
            std::snprintf(Line, sizeof Line, "alarms: %llu, late %llu us (most %llu)", static_cast<unsigned long long>(g.Alarms),
                          static_cast<unsigned long long>(g.Late / g.Alarms), static_cast<unsigned long long>(g.LateMost));
            Say(Line);
        }
        AreasMeasured();
    }
    g.Keys = g.Taken = g.TakenMost = g.Glassed = g.GlassedMost = g.GlassedCount = 0;
    g.Alarms = g.Late = g.LateMost = 0;
    /* A key no frame followed within the second is not waited for. */
    g.Waiting.clear();
    std::fill(std::begin(g.Spent), std::end(g.Spent), 0);
    g.Frames = g.Composed = g.Flushed = g.Longest = 0;
    g.SecondUs = Now;
}

#endif
