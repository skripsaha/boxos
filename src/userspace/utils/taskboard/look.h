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
    Color         Page;              // behind the panels
    Color         Panel;             // inside a panel
    Color         Bars;              // the lines at the top and the bottom
    Color         Frame;             // a panel's frame
    Color         Title;             // titles, and the chosen program's name
    Color         Text;
    Color         Quiet;             // what is said quietly
    Color         Chosen;            // behind the chosen program
    Color         Marked;            // a marked program's mark
    Color         OnScreen;          // where a program is and how it lives,
    Color         Behind;            //   as programs.h Living
    Color         Keeper;
    Color         OnItsOwn;
    Color         Serving;
    Color         Task;
    Color         Started;           // the journal's lines, as journal.h Happened
    Color         Ended;
    Color         Fell;
    Color         Raised;
    Color         Itself;            //   what the board itself did
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
