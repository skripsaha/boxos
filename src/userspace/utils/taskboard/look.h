/* look.h — what the person set of the board (settings taskboard): read whole,
 * and read again when the file is saved or the Use Context changes. */
#ifndef TASKBOARD_LOOK_H
#define TASKBOARD_LOOK_H

#include <cstddef>
#include <cstdint>

#include "box/types.h"
#include "key_chord.h"
#include "painter/core/color.h"

namespace taskboard {

using Color = painter::core::Color;

// What orders the programs, and how they are grouped.
enum class SortBy { Processor, Memory, Name, Age };
enum class GroupBy { Living, None };

// The keys a person chooses, in the order of Look::Keys; the rest are the
// arrows, Enter, Space, Esc and typing, which no setting takes.
enum BoardKey : std::uint8_t { KeyEnd, KeySort, KeyGroup, KeyHelp, KeyCount };

struct Look {
    std::uint32_t RefreshMs;         // how often the machine is read
    std::uint32_t HistorySeconds;    // how far back the graphs reach
    Color         Cool;              // a load's color: light,
    Color         Warm;              //   halfway,
    Color         Hot;               //   full
    SortBy        Sort;
    GroupBy       Group;
    bool          Strands;           // every program's strands shown under it
    bool          Cores;             // the panels beside the programs
    bool          Machine;
    bool          Journal;
    KeyChord      Keys[KeyCount];
};

Look ReadLook();

// The color of a load from 0 to 1, between the person's three.
Color LoadColor(const Look &Shown, double Load);

}  // namespace taskboard

#endif
