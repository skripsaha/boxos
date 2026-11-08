#ifndef EDIT_HISTORY_H
#define EDIT_HISTORY_H

/* What was done to a text, so it can be undone and done again.
 *
 * A change is the bytes taken out at a place and the bytes put in there. A
 * step is what one undo takes back: one change, a run of typing up to the
 * next blank, a run of erasing, or the many changes of one command — a
 * replace of every match, a shift of several lines. Every step has its own
 * number, so the text is unchanged since a save exactly when the newest step
 * done is the one that was newest then.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "text.h"

namespace edit {

struct Change {
    Position    At;
    std::string Removed;
    std::string Added;
};

/* Which run of small changes a change goes on, when it follows one. */
enum class Merge { Never, Typing, ErasingBack, ErasingOn };

class History {
public:
    /* A change already made to the text, the caret before it and after it. */
    void Record(Change Made, Merge Run, Position Before, Position Ends);
    /* The next change begins a step of its own. */
    void Seal() noexcept { Sealed = true; }

    /* The changes recorded from Begin to End are one step. */
    void Begin(Position Before);
    void End(Position Ends);

    /* Where the caret goes, or nothing when there is nothing to take back. */
    std::optional<Position> Undo(Text &Body);
    std::optional<Position> Redo(Text &Body);

    /* The number of the newest step done; 0 is the text as it was opened. */
    std::uint64_t State() const noexcept { return Done.empty() ? 0 : Done.back().Number; }

private:
    struct Step {
        std::vector<Change> Changes;
        Position            Before;
        Position            Ends;
        Merge               Run    = Merge::Never;
        std::uint64_t       Number = 0;
    };

    static bool Joins(Change &Last, const Change &Made, Merge Run);

    std::vector<Step>   Done;
    std::vector<Step>   Undone;
    std::optional<Step> Open;
    std::uint64_t       Numbered = 0;
    bool                Sealed   = true;
};

}  // namespace edit

#endif
