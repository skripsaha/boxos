/* Undo and redo: steps of changes, joined while they go on one run. */

#include "history.h"

#include <utility>

namespace edit {

/* Whether Made goes on Last: typing goes on where the last typing ended,
 * Backspace takes the character just before what it took last, Delete the one
 * now standing where it took. Joined, Last undoes both at once. */
bool History::Joins(Change &Last, const Change &Made, Merge Run)
{
    switch (Run) {
    case Merge::Typing:
        if (Made.At != After(Last.At, Last.Added)) return false;
        Last.Added += Made.Added;
        Last.Removed += Made.Removed;
        return true;
    case Merge::ErasingBack:
        if (!Made.Added.empty() || !Last.Added.empty() || After(Made.At, Made.Removed) != Last.At) return false;
        Last.At      = Made.At;
        Last.Removed = Made.Removed + Last.Removed;
        return true;
    case Merge::ErasingOn:
        if (!Made.Added.empty() || !Last.Added.empty() || Made.At != Last.At) return false;
        Last.Removed += Made.Removed;
        return true;
    case Merge::Never:
        return false;
    }
    return false;
}

void History::Record(Change Made, Merge Run, Position Before, Position Ends)
{
    Undone.clear();
    if (Open) {
        Open->Changes.push_back(std::move(Made));
        return;
    }
    if (Run != Merge::Never && !Sealed && !Done.empty() && Done.back().Run == Run &&
        Joins(Done.back().Changes.back(), Made, Run)) {
        Done.back().Ends = Ends;
        return;
    }
    Done.push_back(Step{{std::move(Made)}, Before, Ends, Run, ++Numbered});
    Sealed = Run == Merge::Never;
}

void History::Begin(Position Before)
{
    Open.emplace();
    Open->Before = Before;
}

void History::End(Position Ends)
{
    if (!Open) return;
    Step Made = std::move(*Open);
    Open.reset();
    Sealed = true;
    if (Made.Changes.empty()) return;
    Made.Ends   = Ends;
    Made.Number = ++Numbered;
    Undone.clear();
    Done.push_back(std::move(Made));
}

std::optional<Position> History::Undo(Text &Body)
{
    if (Done.empty()) return std::nullopt;
    Step Taken = std::move(Done.back());
    Done.pop_back();
    for (auto Each = Taken.Changes.rbegin(); Each != Taken.Changes.rend(); ++Each) {
        Body.Erase(Each->At, After(Each->At, Each->Added));
        Body.Insert(Each->At, Each->Removed);
    }
    const Position Back = Taken.Before;
    Undone.push_back(std::move(Taken));
    Sealed = true;
    return Back;
}

std::optional<Position> History::Redo(Text &Body)
{
    if (Undone.empty()) return std::nullopt;
    Step Again = std::move(Undone.back());
    Undone.pop_back();
    for (const Change &Each : Again.Changes) {
        Body.Erase(Each.At, After(Each.At, Each.Removed));
        Body.Insert(Each.At, Each.Added);
    }
    const Position Ends = Again.Ends;
    Done.push_back(std::move(Again));
    Sealed = true;
    return Ends;
}

}  // namespace edit
