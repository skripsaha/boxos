/* journal.h — what happened while the board looked: programs started, ended,
 * fallen and raised again, heard on Touch (process:spawned, process:died) and
 * named by the programs the board keeps; and what the board itself did.
 * Nothing of the screen. */
#ifndef TASKBOARD_JOURNAL_H
#define TASKBOARD_JOURNAL_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "box/touch.h"

#include "programs.h"

namespace taskboard {

enum class Happened : std::uint8_t { Began, Ended, Fell, Raised, Board };

// The time of day, hh:mm:ss — or the uptime, +mm:ss, with no clock to ask.
std::string TimeOfDay();

struct Entry {
    std::string At;     // the time of day it was heard, hh:mm:ss
    Happened    What = Happened::Board;
    std::string Text;
};

class Journal {
public:
    Journal();
    ~Journal();
    Journal(const Journal &)            = delete;
    Journal &operator=(const Journal &) = delete;

    // Whether births and deaths are heard: the board's Touch claims stand.
    bool Hears() const noexcept;
    // What was heard since: a death said now, named while Known still has the
    // program; a birth kept until a reading names it.
    void Listen(const Programs &Known);
    // After a reading: the births it names said, a raise among them.
    void Take(const Programs &Known);
    // The board's own word.
    void Say(Happened What, std::string Text);

    const std::deque<Entry> &Entries() const noexcept { return Kept; }

private:
    struct Birth {
        std::uint32_t Pid = 0;
        std::uint32_t Generation = 0;
        std::uint32_t FromPid = 0;          // the program it was started from
        std::uint32_t FromGeneration = 0;
        std::uint32_t Readings = 0;         // readings that did not find it
    };

    TouchTag           Deaths = TOUCH_TAG_INVALID;
    TouchTag           Births = TOUCH_TAG_INVALID;
    std::vector<Birth> Pending;
    std::deque<Entry>  Kept;                // the newest last
};

}  // namespace taskboard

#endif
