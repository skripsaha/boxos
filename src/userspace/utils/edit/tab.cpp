/* One open file: moving the caret, writing through Replace, and the view. */

#include "tab.h"

#include <algorithm>

namespace edit {

Tab::Tab(std::string NameValue, file_id_t FileValue, std::string TagsValue, Text BodyValue)
    : File(FileValue), Name(std::move(NameValue)), Tags(std::move(TagsValue)), Body(std::move(BodyValue))
{
}

void Tab::MarkSaved() noexcept
{
    SavedState = Past.State();
    Born       = false;
    Past.Seal();
}

std::pair<Position, Position> Tab::Selection() const noexcept
{
    if (!Anchor) return {Caret, Caret};
    return *Anchor < Caret ? std::pair{*Anchor, Caret} : std::pair{Caret, *Anchor};
}

/* ---- moving ------------------------------------------------------------- */

void Tab::MoveTo(Position To, bool Extend)
{
    if (Extend) {
        if (!Anchor) Anchor = Caret;
    } else {
        Anchor.reset();
    }
    Caret      = Body.Clamp(To);
    WantColumn = Body.ColumnOf(Caret);
    Past.Seal();
}

/* An arrow with a selection and no Shift goes to the selection's edge. */
void Tab::Left1(bool Extend)
{
    if (!Extend && HasSelection()) MoveTo(Selection().first, false);
    else                           MoveTo(Body.Prior(Caret), Extend);
}

void Tab::Right1(bool Extend)
{
    if (!Extend && HasSelection()) MoveTo(Selection().second, false);
    else                           MoveTo(Body.Next(Caret), Extend);
}

/* Up and Down aim at the column the caret last stood at by moving along a
 * line, so a walk past a short line comes out where it went in. Past the first
 * line is the start of the text, past the last its end. */
void Tab::Vertical(bool Down, bool Extend)
{
    const std::size_t Want = WantColumn;
    if (!Down && Caret.Line == 0) {
        MoveTo({0, 0}, Extend);
        return;
    }
    if (Down && Caret.Line + 1 == Body.LineCount()) {
        LineEnd(Extend);
        return;
    }
    const std::size_t Line = Down ? Caret.Line + 1 : Caret.Line - 1;
    MoveTo({Line, Body.ByteAtColumn(Line, Want)}, Extend);
    WantColumn = Want;
}

/* Home goes to where the line's words begin, and from there to its edge. */
void Tab::LineStart(bool Extend)
{
    const std::size_t Indent = Body.IndentOf(Caret.Line);
    MoveTo({Caret.Line, Caret.Byte == Indent ? 0 : Indent}, Extend);
}

/* A whole screen on: the line at the bottom becomes the one at the top, and
 * the caret moves as far, staying where it stood on the glass. */
void Tab::Page(bool Down, std::size_t Rows, bool Extend)
{
    const std::size_t Step = Rows > 1 ? Rows - 1 : 1;
    const std::size_t Last = Body.LineCount() - 1;
    const std::size_t Want = WantColumn;
    std::size_t       Line;
    if (Down) {
        Top  = std::min(Top + Step, Last);
        Line = std::min(Caret.Line + Step, Last);
    } else {
        Top  = Top > Step ? Top - Step : 0;
        Line = Caret.Line > Step ? Caret.Line - Step : 0;
    }
    MoveTo({Line, Body.ByteAtColumn(Line, Want)}, Extend);
    WantColumn = Want;
}

void Tab::SelectAll()
{
    MoveTo({0, 0}, false);
    MoveTo(Body.End(), true);
}

/* ---- writing ------------------------------------------------------------ */

void Tab::Land(Position At)
{
    Anchor.reset();
    Caret      = Body.Clamp(At);
    WantColumn = Body.ColumnOf(Caret);
}

void Tab::Replace(Position From, Position To, std::string_view Bytes, Merge Run)
{
    const Position Before = Caret;
    std::string    Removed = Body.Erase(From, To);
    const Position Ends    = Body.Insert(From, Bytes);
    Land(Ends);
    Past.Record(Change{From, std::move(Removed), std::string(Bytes)}, Run, Before, Ends);
}

void Tab::Type(char Ch, bool Overtype)
{
    const std::string_view One(&Ch, 1);
    if (HasSelection()) {
        const auto [From, To] = Selection();
        Replace(From, To, One, Merge::Never);
        return;
    }

    /* A word is one undo: a blank typed after something that is not one
     * begins the next. */
    const std::string &Line = Body.Line(Caret.Line);
    if ((Ch == ' ' || Ch == '\t') && Caret.Byte > 0 && KindAt(Line, Body.Prior(Caret).Byte) != Kind::Blank)
        Past.Seal();

    Position To = Caret;
    if (Overtype && Caret.Byte < Line.size()) To = Body.Next(Caret);
    Replace(Caret, To, One, Merge::Typing);
}

/* The new line begins where the words of this one began. */
void Tab::NewLine()
{
    const auto [From, To] = Selection();
    const std::size_t Indent = std::min(Body.IndentOf(From.Line), From.Byte);
    std::string       Bytes  = "\n" + Body.Line(From.Line).substr(0, Indent);
    Replace(From, To, Bytes, Merge::Never);
}

/* In the blanks a line begins with, Backspace takes them back to the stop
 * before; anywhere else, the one character. */
void Tab::Backspace()
{
    if (HasSelection()) {
        const auto [From, To] = Selection();
        Replace(From, To, {}, Merge::Never);
        return;
    }
    if (Caret == Position{}) return;

    const std::string &Line = Body.Line(Caret.Line);
    Position           From = Body.Prior(Caret);
    const bool         OnlySpaces =
        Caret.Byte > 0 && std::all_of(Line.begin(), Line.begin() + static_cast<std::ptrdiff_t>(Caret.Byte),
                                      [](char Ch) { return Ch == ' '; });
    if (OnlySpaces) {
        const std::size_t Back = Caret.Byte % TabStop == 0 ? TabStop : Caret.Byte % TabStop;
        From                   = {Caret.Line, Caret.Byte - Back};
    }
    Replace(From, Caret, {}, Merge::ErasingBack);
}

void Tab::Delete()
{
    if (HasSelection()) {
        const auto [From, To] = Selection();
        Replace(From, To, {}, Merge::Never);
        return;
    }
    if (Caret == Body.End()) return;
    Replace(Caret, Body.Next(Caret), {}, Merge::ErasingOn);
}

std::pair<std::size_t, std::size_t> Tab::TouchedLines() const noexcept
{
    const auto [From, To] = Selection();
    /* A selection that ends at the very start of a line does not reach into it. */
    const std::size_t Last = To.Line > From.Line && To.Byte == 0 ? To.Line - 1 : To.Line;
    return {From.Line, Last};
}

/* With nothing selected Tab puts blanks up to the next stop; with a selection
 * every line it reaches moves right one stop, and stays selected. */
void Tab::Indent()
{
    if (!HasSelection()) {
        const std::size_t Room = TabStop - Body.ColumnOf(Caret) % TabStop;
        Replace(Caret, Caret, std::string(Room, ' '), Merge::Never);
        return;
    }

    const auto [First, Last] = TouchedLines();
    const std::string Stop(TabStop, ' ');
    Position          KeptAnchor = *Anchor, KeptCaret = Caret;
    Past.Begin(Caret);
    for (std::size_t Line = First; Line <= Last; Line++) {
        if (Body.Line(Line).empty()) continue;
        Body.Insert({Line, 0}, Stop);
        Past.Record(Change{{Line, 0}, {}, Stop}, Merge::Never, Caret, Caret);
        if (KeptAnchor.Line == Line && KeptAnchor.Byte > 0) KeptAnchor.Byte += TabStop;
        if (KeptCaret.Line == Line && KeptCaret.Byte > 0) KeptCaret.Byte += TabStop;
    }
    Caret  = KeptCaret;
    Anchor = KeptAnchor;
    Past.End(Caret);
}

/* Every line touched moves left one stop: up to four blanks, or one tab. */
void Tab::Unindent()
{
    const auto [First, Last] = TouchedLines();
    std::optional<Position> KeptAnchor = Anchor;
    Position                KeptCaret  = Caret;
    Past.Begin(Caret);
    for (std::size_t Line = First; Line <= Last; Line++) {
        const std::string &Words = Body.Line(Line);
        std::size_t        Cut   = 0;
        if (!Words.empty() && Words[0] == '\t') Cut = 1;
        else while (Cut < TabStop && Cut < Words.size() && Words[Cut] == ' ') Cut++;
        if (Cut == 0) continue;
        std::string Removed = Body.Erase({Line, 0}, {Line, Cut});
        Past.Record(Change{{Line, 0}, std::move(Removed), {}}, Merge::Never, Caret, Caret);
        if (KeptAnchor && KeptAnchor->Line == Line) KeptAnchor->Byte -= std::min(Cut, KeptAnchor->Byte);
        if (KeptCaret.Line == Line) KeptCaret.Byte -= std::min(Cut, KeptCaret.Byte);
    }
    Caret      = KeptCaret;
    Anchor     = KeptAnchor;
    WantColumn = Body.ColumnOf(Caret);
    Past.End(Caret);
}

void Tab::Paste(const Clip &From)
{
    if (From.Bytes.empty()) return;
    if (From.WholeLine && !HasSelection()) {
        const Position    Kept  = Caret;
        const std::size_t Lines = static_cast<std::size_t>(std::count(From.Bytes.begin(), From.Bytes.end(), '\n'));
        Replace({Kept.Line, 0}, {Kept.Line, 0}, From.Bytes, Merge::Never);
        Land({Kept.Line + Lines, Kept.Byte});
        return;
    }
    const auto [Start, Stop] = Selection();
    Replace(Start, Stop, From.Bytes, Merge::Never);
}

Clip Tab::Copy() const
{
    if (HasSelection()) return {Selected(), false};
    return {Body.Line(Caret.Line) + "\n", true};
}

/* With nothing selected Ctrl+X takes the caret's whole line. */
void Tab::Cut()
{
    if (HasSelection()) {
        const auto [From, To] = Selection();
        Replace(From, To, {}, Merge::Never);
        return;
    }
    const std::size_t Line = Caret.Line;
    if (Line + 1 < Body.LineCount()) Replace({Line, 0}, {Line + 1, 0}, {}, Merge::Never);
    else if (Line > 0)               Replace({Line - 1, Body.Line(Line - 1).size()}, {Line, Body.Line(Line).size()}, {}, Merge::Never);
    else                             Replace({0, 0}, {0, Body.Line(0).size()}, {}, Merge::Never);
    Land({std::min(Line, Body.LineCount() - 1), 0});
}

bool Tab::Undo()
{
    std::optional<Position> Back = Past.Undo(Body);
    if (!Back) return false;
    Land(*Back);
    return true;
}

bool Tab::Redo()
{
    std::optional<Position> Again = Past.Redo(Body);
    if (!Again) return false;
    Land(*Again);
    return true;
}

/* ---- the view ----------------------------------------------------------- */

void Tab::Follow(std::size_t Rows, std::size_t Cols) noexcept
{
    if (Rows == 0 || Cols == 0) return;
    Top = std::min(Top, Body.LineCount() - 1);
    if (Caret.Line < Top) Top = Caret.Line;
    else if (Caret.Line >= Top + Rows) Top = Caret.Line - Rows + 1;

    /* Sideways the view jumps a quarter of the width past the caret, so a
     * line typed on past the edge is not followed one column at a time. */
    const std::size_t Column = Body.ColumnOf(Caret);
    const std::size_t Margin = Cols / 4;
    if (Column < Left) Left = Column > Margin ? Column - Margin : 0;
    else if (Column >= Left + Cols) Left = Column - Cols + 1 + Margin;
}

void Tab::Centre(std::size_t Rows) noexcept
{
    Top = Caret.Line > Rows / 2 ? Caret.Line - Rows / 2 : 0;
}

}  // namespace edit
