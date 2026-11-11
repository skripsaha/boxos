/* mosaic.cpp — memory cut into cells. */

#include "mosaic.h"

#include <algorithm>

namespace memmap {

namespace {

constexpr std::uint64_t PageBytes = 4096;

std::uint64_t CellsFor(std::uint64_t Bytes, std::uint64_t CellBytes) noexcept
{
    return Bytes == 0 ? 0 : (Bytes + CellBytes - 1) / CellBytes;
}

// The least cell, a whole number of pages growing by an eighth at a time, that
// Needed(CellBytes) cells of fit in Capacity: so that the glass is filled, and
// not left half empty by a cell that doubled.
template <typename Counter>
std::uint64_t FittingCellBytes(std::size_t Capacity, Counter Needed)
{
    std::uint64_t CellBytes = PageBytes;
    while (Needed(CellBytes) > Capacity && CellBytes < (std::uint64_t{1} << 62))
        CellBytes += std::max(PageBytes, CellBytes / 8 / PageBytes * PageBytes);
    return CellBytes;
}

}  // namespace

Mosaic LayOutOwners(const std::vector<Owner> &Owners, std::size_t Capacity)
{
    Mosaic Laid;
    Laid.View      = ViewOf::Owners;
    Laid.CellBytes = FittingCellBytes(Capacity, [&](std::uint64_t CellBytes) {
        std::uint64_t Sum = 0;
        for (const Owner &One : Owners) Sum += CellsFor(One.Bytes, CellBytes);
        return Sum;
    });
    for (std::size_t Index = 0; Index < Owners.size(); Index++) {
        const std::uint64_t Count = CellsFor(Owners[Index].Bytes, Laid.CellBytes);
        const std::uint64_t Fresh = std::min(Count, CellsFor(Owners[Index].Glow, Laid.CellBytes));
        Laid.Run.push_back(static_cast<std::uint32_t>(Count));
        for (std::uint64_t Nth = 0; Nth < Count && Laid.Cells.size() < Capacity; Nth++)
            Laid.Cells.push_back(Cell{static_cast<std::uint32_t>(Index), static_cast<std::uint32_t>(Nth),
                                      Nth + Fresh >= Count});
    }
    return Laid;
}

Mosaic LayOutAddress(const std::vector<Owner> &Owners, const std::vector<Segment> &Parts, std::uint64_t Span,
                     std::size_t Capacity)
{
    Mosaic Laid;
    Laid.View      = ViewOf::Address;
    Laid.CellBytes = FittingCellBytes(Capacity, [&](std::uint64_t CellBytes) { return CellsFor(Span, CellBytes); });
    Laid.Run.assign(Owners.size(), 0);

    std::vector<std::ptrdiff_t> Whose;
    Whose.reserve(Parts.size());
    for (const Segment &One : Parts) Whose.push_back(OwnerOfRegion(Owners, *One.Region));

    // An owner the registry names outweighs the zone's memory, whatever the
    // bytes: a ring of three pages is not lost in a cell of a thousand.
    const auto Named = [&](std::size_t Owner) { return Owners[Owner].What != Kind::Untold; };
    std::vector<std::uint64_t> Held(Owners.size(), 0);
    std::vector<std::size_t>   Touched;
    std::size_t                First = 0;
    const std::uint64_t        Count = CellsFor(Span, Laid.CellBytes);
    for (std::uint64_t Index = 0; Index < Count; Index++) {
        const std::uint64_t From = Index * Laid.CellBytes, To = From + Laid.CellBytes;
        while (First < Parts.size() && Parts[First].Base + Parts[First].Bytes <= From) First++;
        Touched.clear();
        for (std::size_t Part = First; Part < Parts.size() && Parts[Part].Base < To; Part++) {
            if (Whose[Part] < 0) continue;
            const std::size_t Owner = static_cast<std::size_t>(Whose[Part]);
            if (Held[Owner] == 0) Touched.push_back(Owner);
            Held[Owner] += std::min(To, Parts[Part].Base + Parts[Part].Bytes) - std::max(From, Parts[Part].Base);
        }
        Cell Here;
        for (const std::size_t Owner : Touched) {
            const bool Better = Here.Owner == NoOwner || (Named(Owner) && !Named(Here.Owner)) ||
                                (Named(Owner) == Named(Here.Owner) && Held[Owner] > Held[Here.Owner]);
            if (Better) Here.Owner = static_cast<std::uint32_t>(Owner);
        }
        for (const std::size_t Owner : Touched) Held[Owner] = 0;
        if (Here.Owner != NoOwner) Laid.Run[Here.Owner]++;
        Laid.Cells.push_back(Here);
    }
    return Laid;
}

std::vector<Segment> PartsWithin(const std::vector<Segment> &Parts, std::uint64_t From, std::uint64_t To)
{
    std::vector<Segment> Inside;
    for (const Segment &One : Parts) {
        const std::uint64_t Begin = std::max(From, One.Base), End = std::min(To, One.Base + One.Bytes);
        if (Begin < End) Inside.push_back(Segment{Begin, End - Begin, One.Region});
    }
    return Inside;
}

}  // namespace memmap
