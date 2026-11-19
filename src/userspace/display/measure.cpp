#include "measure.h"

#if defined(BOX_MEASURE)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "box/clock.h"
#include "box/debug.h"
#include "boxos_pixel.h"
#include "areas.h"
#include "easel.h"
#include "mirror.h"
#include "painter/core/canvas.h"
#include "painter/core/cpu_path.h"
#include "painter/core/fill.h"
#include "painter/core/mask.h"
#include "painter/core/path.h"
#include "painter/core/picture.h"
#include "painter/core/shadow.h"
#include "painter/core/transform.h"

namespace {

using painter::core::Canvas;
using Paint = painter::core::Color;
using painter::core::CpuPath;
using painter::core::Path;
using painter::core::Rect;
using painter::core::RectF;
using painter::core::Transform;

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

/* The counter's ticks a microsecond: counted against the kernel's clock over
 * a fifth of a second, from one of its steps to another. */
double TicksPerUs() noexcept
{
    const std::uint64_t First = clock_uptime_us();
    std::uint64_t       From  = First;
    while ((From = clock_uptime_us()) == First) {
    }
    const std::uint64_t Began = Ticks();
    std::uint64_t       To    = From;
    while ((To = clock_uptime_us()) < From + 200000) {
    }
    return static_cast<double>(Ticks() - Began) / static_cast<double>(To - From);
}

/* Nanoseconds a pixel of Work, which covers Pixels: the fastest of a few
 * runs after a first that warms it. */
template <typename Working> double NsPerPixel(Working &&Work, double Pixels)
{
    Work();
    std::uint64_t Least = ~std::uint64_t{0};
    for (int Run = 0; Run < 12; Run++) {
        const std::uint64_t Began = Ticks();
        Work();
        Least = std::min(Least, Ticks() - Began);
    }
    return static_cast<double>(Least) / g.TicksPerUs * 1000.0 / Pixels;
}

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
     * enough to be known better than at start. */
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

void MeasureEngine(Easel &Glass) noexcept
{
    g.TicksPerUs = TicksPerUs();
    constexpr std::uint32_t Side = 256;
    constexpr double        All  = static_cast<double>(Side) * Side;
    std::optional<Canvas>   Tile = Canvas::Make(Side, Side), Kept = Canvas::Make(Side, Side), Other = Canvas::Make(Side, Side);
    if (!Tile || !Kept || !Other) {
        Say("engine: no room to measure it");
        return;
    }
    try {
        Kept->Clear(Paint::Rgba(40, 120, 200, 255));
        Kept->Fill(Path::Circle({128, 128}, 90), Paint::Rgba(250, 200, 60, 255));
        Other->Clear(Paint::Rgba(200, 60, 90, 255));
        std::vector<painter::core::Pixel> Pixels;
        for (std::uint32_t Y = 0; Y < Side; Y++) Pixels.insert(Pixels.end(), Kept->Row(Y), Kept->Row(Y) + Side);
        const auto Picture = painter::core::Picture::FromPixels(Pixels, Side, Side, Side);
        const Transform Turn = Transform::Translate(-128, -128).Then(Transform::Rotate(10)).Then(Transform::Translate(128, 128));
        const painter::core::Shadow Cast{{0, 6}, 16, 0, Paint::Rgba(0, 0, 0, 96), false};
        const Path                  Panel = Path::RoundedRect(RectF{24, 24, 208, 208}, painter::core::Corners::All(16));
        std::vector<std::uint32_t>  Out(static_cast<std::size_t>(Side) * Side);
        Mirror                      Into(Side, Side);

        const CpuPath Was = painter::core::ChosenCpuPath();
        for (const CpuPath Way : {CpuPath::Plain, CpuPath::Sse2, CpuPath::Avx2}) {
            if (!painter::core::UseCpuPath(Way)) continue;
            const Paint Through = Paint::Rgba(30, 160, 90, 200);
            const double Measured[] = {
                NsPerPixel([&] { Tile->Clear(Paint::Rgba(20, 20, 30, 255)); }, All),
                NsPerPixel([&] { Tile->Draw(*Kept, {0, 0}, 1.0f); }, All),
                NsPerPixel([&] { Tile->Draw(*Kept, {0, 0}, 0.5f); }, All),
                NsPerPixel([&] { (void)Tile->Fill(Path::Rect(RectF{0, 0, Side, Side}), Through); }, All),
                NsPerPixel([&] { (void)Tile->Fill(Path::RoundedRect(RectF{0, 0, Side, Side}, painter::core::Corners::All(24)), Through); }, All),
                NsPerPixel([&] { (void)Tile->Fill(Path::Circle({128, 128}, 128), Through); }, 3.14159 * 128 * 128),
                NsPerPixel([&] {
                    if (const auto Mask = Cast.MaskFor(Panel)) Tile->DrawShadow(*Mask, Cast);
                }, All),
                NsPerPixel([&] { (void)Tile->Fill(Path::Rect(RectF{0, 0, Side, Side}), painter::core::Fill::Picture(Picture), Turn); }, All),
                NsPerPixel([&] { Other->FadeTo(*Kept, 0.5f); }, All),
                NsPerPixel([&] { Tile->CopyOut(Rect{0, 0, Side, Side}, Out.data(), Side, BOX_PIXEL_BGR, painter::core::Dither::On); }, All),
                NsPerPixel([&] { Tile->CopyOut(Rect{0, 0, Side, Side}, Out.data(), Side, BOX_PIXEL_BGR, painter::core::Dither::Off); }, All),
                NsPerPixel([&] { Into.Put(*Tile, Rect{0, 0, Side, Side}); }, All),
            };
            char Line[256];
            std::snprintf(Line, sizeof Line,
                          "engine %s ns/px: clear %s opaque %s half %s rect %s round %s circle %s shadow %s turned %s fade %s dither %s "
                          "nearest %s put %s",
                          painter::core::CpuPathName(Way), Fixed(Measured[0]).Text, Fixed(Measured[1]).Text, Fixed(Measured[2]).Text,
                          Fixed(Measured[3]).Text, Fixed(Measured[4]).Text, Fixed(Measured[5]).Text, Fixed(Measured[6]).Text,
                          Fixed(Measured[7]).Text, Fixed(Measured[8]).Text, Fixed(Measured[9]).Text, Fixed(Measured[10]).Text,
                          Fixed(Measured[11]).Text);
            Say(Line);
        }
        (void)painter::core::UseCpuPath(Was);

        /* What the shadow's coverage costs to make, a tile's canvas to get,
         * memory to copy, and the glass to take a whole picture. */
        const double Mask = NsPerPixel([&] { (void)Cast.MaskFor(Panel); }, 1.0) / 1000.0;
        const double Make = NsPerPixel([&] { (void)Canvas::Make(Side, Side); }, 1.0) / 1000.0;
        std::vector<std::uint32_t> From(1u << 20), To(1u << 20);
        const double Copy = NsPerPixel([&] { std::memcpy(To.data(), From.data(), From.size() * 4); }, static_cast<double>(From.size()));
        const std::uint32_t Wide = Glass.PixelWidth(), High = Glass.PixelHeight();
        const double        Put  = NsPerPixel([&] {
            Glass.Picture().Refresh();
            Glass.Flush();
        }, static_cast<double>(Wide) * High);
        char Line[256];
        std::snprintf(Line, sizeof Line,
                      "engine: a shadow's coverage %s us, a %ux%u canvas got %s us, memory copied %s ns/px, the glass %ux%u taken "
                      "whole %s ns/px (%s ms), the counter %s ticks a us",
                      Fixed(Mask).Text, Side, Side, Fixed(Make).Text, Fixed(Copy).Text, Wide, High, Fixed(Put).Text,
                      Fixed(Put * Wide * High / 1e6).Text, Fixed(g.TicksPerUs).Text);
        Say(Line);
    } catch (...) {
        Say("engine: no room to measure it all");
    }
}

#endif
