#ifndef EDIT_TAB_H
#define EDIT_TAB_H

/* One open file: its text and what was done to it, where the caret and the
 * selection are, which part of it is in view, and whether it may be written.
 *
 * Every change of the text goes through Replace, so every change is in the
 * history and leaves the caret after what it put in.
 */

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "box/file.h"

#include "history.h"
#include "text.h"

namespace edit {

/* What Ctrl+C and Ctrl+X took. A whole line is taken when nothing is
 * selected, and is put back as a line of its own, above the caret's. */
struct Clip {
    std::string Bytes;
    bool        WholeLine = false;
};

class Tab {
public:
    Tab(std::string Name, file_id_t File, std::string Tags, Text Body);

    file_id_t   File = 0;
    std::string Name;
    std::string Tags;          /* as the volume says them, comma-joined */
    bool        Safety = false;
    bool        Born   = false; /* made by this edit and never written: let go with its tab */

    Text    Body;
    History Past;

    Position                Caret;
    std::optional<Position> Anchor;      /* the other end of the selection */
    std::size_t             WantColumn = 0;
    std::size_t             Top = 0;     /* the first line in view */
    std::size_t             Left = 0;    /* the first column in view */

    bool Dirty() const noexcept { return Past.State() != SavedState; }
    void MarkSaved() noexcept;

    bool                          HasSelection() const noexcept { return Anchor && *Anchor != Caret; }
    std::pair<Position, Position> Selection() const noexcept;
    std::string                   Selected() const { return Body.Slice(Selection().first, Selection().second); }

    /* Moving. Extend keeps the anchor where it was, so the selection grows. */
    void MoveTo(Position To, bool Extend);
    void Left1(bool Extend);
    void Right1(bool Extend);
    void Vertical(bool Down, bool Extend);
    void WordLeft(bool Extend) { MoveTo(Body.WordBefore(Caret), Extend); }
    void WordRight(bool Extend) { MoveTo(Body.WordAfter(Caret), Extend); }
    void LineStart(bool Extend);
    void LineEnd(bool Extend) { MoveTo({Caret.Line, Body.Line(Caret.Line).size()}, Extend); }
    void Page(bool Down, std::size_t Rows, bool Extend);
    void SelectAll();

    /* Writing. */
    void Replace(Position From, Position To, std::string_view Bytes, Merge Run);
    void Type(char Ch, bool Overtype);
    void NewLine();
    void Backspace();
    void Delete();
    void Indent();
    void Unindent();
    void Paste(const Clip &From);
    Clip Copy() const;
    Clip Cut();
    bool Undo();
    bool Redo();

    /* The view moved so the caret is in it; Centre puts its line mid-screen. */
    void Follow(std::size_t Rows, std::size_t Cols) noexcept;
    void Centre(std::size_t Rows) noexcept;

private:
    /* The lines a command on lines works on: the caret's, or every line the
     * selection reaches into. */
    std::pair<std::size_t, std::size_t> TouchedLines() const noexcept;
    void                                Land(Position At);

    std::uint64_t SavedState = 0;
};

}  // namespace edit

#endif
