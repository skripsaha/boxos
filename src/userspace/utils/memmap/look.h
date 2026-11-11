/* look.h — what the person set of the map (settings memmap): read whole, and
 * read again when the file is saved or the Use Context changes. */
#ifndef MEMMAP_LOOK_H
#define MEMMAP_LOOK_H

#include <cstddef>
#include <cstdint>

#include "key_chord.h"
#include "painter/core/color.h"

namespace memmap {

using Color = painter::core::Color;

// What the map shows: memory laid out by who holds it, or the address space
// as the registry of tagged regions tells it.
enum class ViewOf : std::uint8_t { Owners, Address };
// Each program apart, or all programs as one kind.
enum class GroupBy : std::uint8_t { Program, Kind };
enum class SortBy : std::uint8_t { Size, Age, Name };

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, Space, Esc and typing, which no setting takes.
enum MapKey : std::uint8_t { KeyView, KeyGroup, KeySort, KeyPause, KeyHelp, KeyCount };

struct Look {
    std::uint32_t RefreshMs;     // how often the machine is read
    std::uint32_t CellPixels;    // one cell's side with its gap, in pixels
    std::uint32_t Tint;          // how rich the colors made for programs are, in percent
    Color         Page;          // behind the panels
    Color         Panel;         // inside a panel
    Color         Bars;          // the lines at the top and the bottom
    Color         Frame;         // a panel's frame
    Color         Title;         // titles, and the chosen owner's name
    Color         Text;
    Color         Quiet;         // what is said quietly
    Color         Marked;        // a marked program's mark
    Color         Image;         // the kinds of memory, as machine.h Kind
    Color         Kernel;
    Color         Rings;
    Color         Shared;
    Color         Device;
    Color         Programs;      // all programs, when they are one kind
    Color         Free;
    Color         Untold;
    Color         Grew;          // what an owner holds more of, and less of
    Color         Shrank;
    ViewOf        View;
    GroupBy       Group;
    SortBy        Sort;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

}  // namespace memmap

#endif
