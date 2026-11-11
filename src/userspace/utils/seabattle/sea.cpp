/* sea.cpp — the water's picture (sea.h). The swell drifts slowly, so each patch
 * of water is worked out once for every SwellMs and copied in between: the
 * glass is mostly sea and must cost little while nothing else moves. */

#include "sea.h"

#include <algorithm>
#include <vector>

namespace seabattle {

namespace {

constexpr std::uint64_t SwellMs = 200;
constexpr std::size_t   MostKept = 6;

struct Kept {
    SeaPatch           Patch;
    std::uint64_t      Step = 0;
    std::vector<Color> Pixels;
};

std::vector<Kept> Patches;

bool Same(const SeaPatch &One, const SeaPatch &Other)
{
    return One.X == Other.X && One.Y == Other.Y && One.Width == Other.Width && One.Height == Other.Height && One.Surface == Other.Surface &&
           One.Depth == Other.Depth && One.Swell == Other.Swell && One.Flat == Other.Flat;
}

void Work(Kept &Water, std::uint64_t Step)
{
    const SeaPatch &Sea = Water.Patch;
    const Color     Deep = Darker(Sea.Surface, Sea.Depth);
    const auto      Time = static_cast<float>(Step % 18000) * static_cast<float>(SwellMs) * 0.001f;
    const auto      Tick = static_cast<std::uint32_t>(Step);
    Water.Step = Step;
    Water.Pixels.resize(static_cast<std::size_t>(Sea.Width) * Sea.Height);
    for (int Y = 0; Y < Sea.Height; Y++) {
        const Color Base = Mixed(Sea.Surface, Deep, static_cast<float>(Y) / static_cast<float>(Sea.Height));
        for (int X = 0; X < Sea.Width; X++) {
            Color Ink = Base;
            if (Sea.Flat) {
                Ink = ((X / 3 + Y / 2) & 1) != 0 ? Base : Darker(Base, 0.12f);
            } else {
                const auto  Across = static_cast<float>(Sea.X + X), Down = static_cast<float>(Sea.Y + Y);
                const float Swell = 0.55f * Wave(Down * 0.17f + Across * 0.035f + Time * 0.11f) +
                                    0.45f * Wave(Down * 0.24f - Across * 0.052f - Time * 0.15f + 0.3f);
                Ink = Toned(Base, 1.0f + 0.1f * Swell * Sea.Swell);
                if (Swell > 0.74f) Ink = Lighter(Ink, (Swell - 0.74f) * 0.9f * Sea.Swell);
                if (Swell > 0.6f && Grain(Sea.X + X, Sea.Y + Y, Tick) > 0.9985f) Ink = Lighter(Ink, 0.6f);
            }
            Water.Pixels[static_cast<std::size_t>(Y) * Sea.Width + X] = Ink;
        }
    }
}

}  // namespace

void PaintSea(Canvas &Out, const SeaPatch &Sea, std::uint64_t Ms)
{
    const std::uint64_t Step = Ms / SwellMs;
    auto                Found = std::ranges::find_if(Patches, [&](const Kept &One) { return Same(One.Patch, Sea); });
    if (Found == Patches.end()) {
        if (Patches.size() >= MostKept) Patches.erase(Patches.begin());
        Patches.push_back(Kept{.Patch = Sea, .Step = 0, .Pixels = {}});
        Found = Patches.end() - 1;
        Work(*Found, Step);
    } else if (Found->Step != Step) {
        Work(*Found, Step);
    }
    for (int Y = std::max(0, -Sea.Y); Y < std::min(Sea.Height, Out.Height - Sea.Y); Y++)
        for (int X = std::max(0, -Sea.X); X < std::min(Sea.Width, Out.Width - Sea.X); X++)
            Out.Put(Sea.X + X, Sea.Y + Y, Found->Pixels[static_cast<std::size_t>(Y) * Sea.Width + X]);
}

void PaintBackdrop(Canvas &Out, Color Sea, bool Flat, std::uint64_t Ms)
{
    PaintSea(Out, SeaPatch{0, 0, Out.Width, Out.Height, Darker(Sea, 0.5f), 0.75f, 0.7f, Flat}, Ms);
}

}  // namespace seabattle
