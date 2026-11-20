/*
 * region.cpp — a set of the screen's pixels. See region.h.
 */

#include "region.h"

#include <algorithm>
#include <new>

namespace items {

using painter::core::Rect;

namespace {

struct Span {
    std::int32_t From, To;
};

}  // namespace

void Region::Add(Rect Area) noexcept
{
    if (Area.Empty()) return;
    Bounds = Empty() ? Area : Bounds.Join(Area);
    if (Overflowed) return;
    try {
        Parts.push_back(Area);
    } catch (const std::bad_alloc &) {
        Overflowed = true;
    }
}

/* A sweep down the rows: at every row a part begins or ends, the parts lying
 * across it are those begun and not ended, and their spans across, joined
 * where they touch, are the band's. The parts across are kept in order
 * across as the sweep goes — those that end taken out in place, those that
 * begin put in order among themselves and merged in — so a band costs the
 * parts across it, not a sort of them all again. */
template <typename Telling> void Region::Bands(Telling &&Tell) const
{
    std::vector<std::int32_t> Rows;
    Rows.reserve(Parts.size() * 2);
    for (const Rect &Each : Parts) Rows.push_back(Each.Y), Rows.push_back(Each.Y + Each.Height);
    std::sort(Rows.begin(), Rows.end());
    Rows.erase(std::unique(Rows.begin(), Rows.end()), Rows.end());
    std::vector<Rect> ByTop = Parts;
    std::sort(ByTop.begin(), ByTop.end(), [](const Rect &A, const Rect &B) { return A.Y < B.Y; });

    const auto        Leftward = [](const Rect &A, const Rect &B) { return A.X < B.X; };
    std::vector<Rect> Across;
    std::vector<Span> Spans;
    std::size_t       Next = 0;
    for (std::size_t R = 0; R + 1 < Rows.size(); R++) {
        const std::int32_t Top = Rows[R], Bottom = Rows[R + 1];
        std::erase_if(Across, [Top](const Rect &Each) { return Each.Y + Each.Height <= Top; });
        const std::size_t Kept = Across.size();
        while (Next < ByTop.size() && ByTop[Next].Y == Top) Across.push_back(ByTop[Next++]);
        if (Across.empty()) continue;
        if (Across.size() > Kept) {
            std::sort(Across.begin() + static_cast<std::ptrdiff_t>(Kept), Across.end(), Leftward);
            std::inplace_merge(Across.begin(), Across.begin() + static_cast<std::ptrdiff_t>(Kept), Across.end(), Leftward);
        }
        Spans.clear();
        for (const Rect &Each : Across) {
            if (!Spans.empty() && Each.X <= Spans.back().To) Spans.back().To = std::max(Spans.back().To, Each.X + Each.Width);
            else Spans.push_back(Span{Each.X, Each.X + Each.Width});
        }
        Tell(Top, Bottom, Spans);
    }
}

std::uint64_t Region::Pixels() const
{
    if (Overflowed) return static_cast<std::uint64_t>(Bounds.Width) * static_cast<std::uint64_t>(Bounds.Height);
    std::uint64_t Made = 0;
    Bands([&Made](std::int32_t Top, std::int32_t Bottom, const std::vector<Span> &Spans) {
        for (const Span &Each : Spans) Made += static_cast<std::uint64_t>(Each.To - Each.From) * static_cast<std::uint64_t>(Bottom - Top);
    });
    return Made;
}

std::vector<Rect> Region::Take()
{
    std::vector<Rect> Made;
    if (Overflowed) {
        Made.push_back(Bounds);
    } else {
        /* A span open since Since while the bands under it have the same. */
        struct Open {
            Span         Across;
            std::int32_t Since;
        };
        std::vector<Open> Was, Now;
        std::int32_t      Last = 0;   // where the band before ended
        const auto        Close = [&Made](const Open &One, std::int32_t Until) {
            Made.push_back(Rect{One.Across.From, One.Since, One.Across.To - One.Across.From, Until - One.Since});
        };
        Bands([&](std::int32_t Top, std::int32_t Bottom, const std::vector<Span> &Spans) {
            if (Top != Last) {
                for (const Open &One : Was) Close(One, Last);
                Was.clear();
            }
            Now.clear();
            std::size_t W = 0;
            for (const Span &Each : Spans) {
                while (W < Was.size() && Was[W].Across.From < Each.From) Close(Was[W++], Top);
                if (W < Was.size() && Was[W].Across.From == Each.From && Was[W].Across.To == Each.To) Now.push_back(Was[W++]);
                else Now.push_back(Open{Each, Top});
            }
            while (W < Was.size()) Close(Was[W++], Top);
            std::swap(Was, Now);
            Last = Bottom;
        });
        for (const Open &One : Was) Close(One, Last);
    }
    Clear();
    return Made;
}

void Region::Clear() noexcept
{
    Parts.clear();
    Bounds     = {};
    Overflowed = false;
}

}  // namespace items
