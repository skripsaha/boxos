/* What edit draws: the tabs, the text, the status line, and the windows
 * editor.cpp keeps. */

#include <algorithm>
#include <format>
#include <vector>

#include "kin/volume.h"

#include "editor.h"
#include "text.h"

namespace edit {

namespace {

std::int32_t DigitsOf(std::size_t Number)
{
    std::int32_t Digits = 1;
    for (; Number >= 10; Number /= 10) Digits++;
    return Digits;
}

}  // namespace

/* ---- the editor's picture ----------------------------------------------- */

std::int32_t Editor::GutterWidth() const noexcept
{
    if (Tabs.empty()) return 0;
    return std::max(3, DigitsOf(Active().Body.LineCount())) + 3;
}

std::size_t Editor::TextRows() const noexcept
{
    return Bounds.Height > 2 ? static_cast<std::size_t>(Bounds.Height - 2) : 1;
}

/* The last column is kept for the mark of a line that goes on past it. */
std::size_t Editor::TextCols() const noexcept
{
    const std::int32_t Cols = Bounds.Width - GutterWidth() - 1;
    return Cols > 0 ? static_cast<std::size_t>(Cols) : 1;
}

bool Editor::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({TextInk, Paper});
    std::vector<TabFace> Faces;
    for (const std::unique_ptr<Tab> &One : Tabs) Faces.push_back({One->Name, One->Dirty(), One->Safety});
    PaintTabs(Out, Bounds.Width, Faces, Current);
    if (!Tabs.empty()) PaintText(Out);
    PaintStatus(Out);
    switch (Now) {
    case Mode::Writing: break;
    case Mode::Picking: Chooser.Paint(Out); break;
    case Mode::Finding: Finder.Paint(Out, Active()); break;
    case Mode::GoingTo: PaintGoTo(Out); break;
    case Mode::Helping: Keys.Paint(Out, Bounds); break;
    case Mode::Carding: FileCard.Paint(Out); break;
    case Mode::Clipping: Clipboard.Paint(Out); break;
    case Mode::Leaving: PaintLeaving(Out); break;
    }
    return static_cast<bool>(Screen.Present());
}

void Editor::PaintText(tui::Frame &Out)
{
    Tab              &Here = Active();
    const std::size_t Rows = TextRows();
    for (std::size_t Row = 0; Row < Rows && Here.Top + Row < Here.Body.LineCount(); Row++)
        PaintLine(Out, static_cast<std::int32_t>(Row + 1), Here.Top + Row);

    if (Now != Mode::Writing) return;
    const std::size_t Column = Here.Body.ColumnOf(Here.Caret);
    if (Here.Caret.Line < Here.Top || Here.Caret.Line >= Here.Top + Rows || Column < Here.Left ||
        Column >= Here.Left + TextCols())
        return;
    const core::Point At{GutterWidth() + static_cast<std::int32_t>(Column - Here.Left),
                         1 + static_cast<std::int32_t>(Here.Caret.Line - Here.Top)};

    /* Typing over: the caret is a block the size of the character it takes. */
    if (Overtype) {
        tui::Cell Under = Out.Read(At);
        Under.Foreground = BlockInk;
        Under.Background = BlockPaper;
        (void)Out.Put(At, Under);
        return;
    }
    (void)Out.SetCaret(At);
}

void Editor::PaintLine(tui::Frame &Out, std::int32_t Y, std::size_t Line)
{
    Tab               &Here    = Active();
    const std::string &Words   = Here.Body.Line(Line);
    const bool         AtCaret = Line == Here.Caret.Line;
    const core::Color  Under   = AtCaret ? LinePaper : Paper;
    const std::int32_t Gutter  = GutterWidth();
    const std::size_t  Cols    = TextCols();
    Out.Fill({Gutter, Y, Bounds.Width - Gutter, 1}, tui::Cell{' ', TextInk, Under});

    /* The numbers stand on the plain paper, the caret's one brighter: the
     * line it is on is lit only where its text is. */
    const std::string Number = std::to_string(Line + 1);
    tui::PutText(Out, {Gutter - 3 - static_cast<std::int32_t>(Number.size()), Y}, Number, Gutter,
            AtCaret ? CaretNumberInk : NumberInk, Paper);
    (void)Out.Put({Gutter - 2, Y}, Here.Left > 0 && !Words.empty() ? tui::Glyphs::MoreLeft : tui::Glyphs::RuleDown, {RuleInk, Paper});

    /* The matches on this line, while the find window is open. */
    struct Mark { std::size_t From, To; bool Chosen; };
    std::vector<Mark> Marks;
    if (Now == Mode::Finding && Finder.NeedleBytes() > 0) {
        const std::vector<Position> &All = Finder.Matches(Here);
        for (auto Each = std::lower_bound(All.begin(), All.end(), Position{Line, 0});
             Each != All.end() && Each->Line == Line; ++Each)
            Marks.push_back({Each->Byte, Each->Byte + Finder.NeedleBytes(), *Each == Here.Caret});
    }
    const auto [SelectFrom, SelectTo] = Here.Selection();
    auto Selected = [&](std::size_t Byte) {
        const Position At{Line, Byte};
        return Here.HasSelection() && SelectFrom <= At && At < SelectTo;
    };

    std::size_t Column = 0, Byte = 0;
    while (Byte < Words.size() && Column < Here.Left + Cols) {
        const tui::Glyph Shape = tui::GlyphAt(Words, Byte, Column);
        const Mark *Over  = nullptr;
        for (const Mark &One : Marks)
            if (Byte >= One.From && Byte < One.To) Over = &One;
        const core::Color Ground = Selected(Byte) ? SelectedPaper : Over ? (Over->Chosen ? HerePaper : MatchPaper) : Under;
        const core::Color Ink    = Over && Over->Chosen ? HereInk : Shape.Odd ? tui::OddInk : TextInk;
        const bool        Blank  = Words[Byte] == '\t';
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++, Column++)
            if (Column >= Here.Left && Column < Here.Left + Cols)
                (void)Out.Put({Gutter + static_cast<std::int32_t>(Column - Here.Left), Y},
                              tui::Cell{Blank ? ' ' : Shape.Cells[Cell], Ink, Ground});
        Byte += Shape.Length;
    }

    /* A selection that goes on to the next line takes this line's end. */
    if (Byte == Words.size() && Selected(Byte) && Column >= Here.Left && Column < Here.Left + Cols)
        (void)Out.Put({Gutter + static_cast<std::int32_t>(Column - Here.Left), Y}, ' ', {TextInk, SelectedPaper});
    if (Byte < Words.size() || Column > Here.Left + Cols)
        (void)Out.Put({Gutter + static_cast<std::int32_t>(Cols), Y}, tui::Glyphs::MoreRight, {RuleInk, Under});
}

/* Left: what the tab is and where the caret stands; right: what edit has just
 * said, or the way to the keys. */
void Editor::PaintStatus(tui::Frame &Out) const
{
    StatusLine Line(Out, Bounds, Message, MessageTrouble);
    if (Tabs.empty()) {
        Line.Put("no file open", BarInk);
        return;
    }

    const Tab &Here = Active();
    if (Looking == Scope::Everywhere) Line.Badge("EVERYWHERE", EverywherePaper);
    if (Here.Safety)                  Line.Badge("SAFETY", SafetyPaper);
    if (Overtype)                     Line.Badge("OVERTYPE", OvertypePaper);
    Line.Put(tui::Shortened(Here.Name, 32), ShownTabInk);
    if (Here.Born) Line.Put("(new)", tui::HintInk);

    std::string Place = std::format("{}:{}", Here.Caret.Line + 1, Here.Body.ColumnOf(Here.Caret) + 1);
    if (Here.HasSelection()) {
        const auto [From, To] = Here.Selection();
        Place += From.Line == To.Line ? std::format("  {} bytes selected", To.Byte - From.Byte)
                                      : std::format("  {} lines selected", To.Line - From.Line + 1);
    }
    if (Here.Body.Crlf()) Place += "  CRLF";
    Line.Put(Place, BarInk);

    const std::vector<std::string> Worn = TagWords(Here.Tags);
    if (!Worn.empty())
        Line.Put(tui::Shortened("[" + Joined(Worn, ", ") + "]", static_cast<std::size_t>(Line.Room())), tui::HintInk);
}

void Editor::PaintGoTo(tui::Frame &Out) const
{
    const std::int32_t Width  = std::min(Bounds.Width - 2, 40);
    const core::Rect   Inside = tui::DrawWindow(Out, {Bounds.Width - Width - 1, 1, Width, 3}, "Go to line",
                                           std::format("of {}", Active().Body.LineCount()), "line or line:column");
    LineField.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, true);
}

void Editor::PaintLeaving(tui::Frame &Out) const
{
    const std::size_t Room   = Bounds.Height > 6 ? static_cast<std::size_t>(Bounds.Height - 6) : 1;
    const std::size_t Listed = std::min(Unsaved.size(), Room);
    const bool        More   = Listed < Unsaved.size();
    const core::Rect  Area   = tui::Centred(Bounds, std::min(Bounds.Width - 4, 60),
                                       static_cast<std::int32_t>(Listed + (More ? 1 : 0)) + 2);
    const core::Rect  Inside = tui::DrawWindow(Out, Area, "Changes not saved", "",
                                          LeavingAll ? "S save all  D leave without saving  Esc stay"
                                                     : "S save  D close without saving  Esc keep it");
    for (std::size_t Index = 0; Index < Listed; Index++) {
        const Tab &One = *Unsaved[Index];
        tui::PutText(Out, {Inside.X, Inside.Y + static_cast<std::int32_t>(Index)},
                One.Born ? One.Name + "  (new)" : One.Name, Inside.Width, tui::TitleInk, tui::WindowPaper);
    }
    if (More)
        tui::PutText(Out, {Inside.X, Inside.Y + static_cast<std::int32_t>(Listed)},
                std::format("and {} more", Unsaved.size() - Listed), Inside.Width, tui::HintInk, tui::WindowPaper);
}

}  // namespace edit
