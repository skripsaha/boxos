/* mosaic.h — memory cut into cells, each a run of pages with one owner: laid
 * out owner after owner, so that what an owner holds grows or shrinks where
 * the eye is; or in address order, so that a cell is where it is in memory.
 * Nothing of the screen but how many cells it has room for. */
#ifndef MEMMAP_MOSAIC_H
#define MEMMAP_MOSAIC_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "owners.h"

namespace memmap {

inline constexpr std::uint32_t NoOwner = UINT32_MAX;

struct Cell {
    std::uint32_t Owner = NoOwner;    // an index into the owners; NoOwner where nothing is
    std::uint32_t Nth = 0;            // by owner: which of its cells this is
    bool          Fresh = false;      // by owner: held just now, and not before
};

struct Mosaic {
    ViewOf             View = ViewOf::Owners;
    std::vector<Cell>  Cells;
    std::uint64_t      CellBytes = 0;     // the memory one cell stands for
    std::vector<std::uint32_t> Run;       // by owner: how many cells each has

    // By address: the first byte of cell Index.
    std::uint64_t Base(std::size_t Index) const noexcept { return Index * CellBytes; }
};

// The owners' cells one after the other, Total bytes cut so that every owner
// that holds anything has at least one of the Capacity cells there are room for.
Mosaic LayOutOwners(const std::vector<Owner> &Owners, std::size_t Capacity);
// The address space up to Span in cells of equal size; a cell is its owner's
// who holds the most of it, an owner the registry names before the zone does.
Mosaic LayOutAddress(const std::vector<Owner> &Owners, const std::vector<Segment> &Parts, std::uint64_t Span,
                     std::size_t Capacity);

// What lies within [From, To), cut to it.
std::vector<Segment> PartsWithin(const std::vector<Segment> &Parts, std::uint64_t From, std::uint64_t To);

}  // namespace memmap

#endif
