/* What edit draws: the shared ways first, then the editor's own picture — the
 * tabs, the text, the status line, and the windows editor.cpp keeps. */

#include "paint.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

#include "painter/tui/glyph.h"

#include "editor.h"
#include "keymap.h"
#include "text.h"
#include "volume.h"

namespace edit {

namespace {

/* A shortened tab name keeps five letters before its "...": fewer, and the
 * tabs of a project's files all read alike. */
constexpr std::int32_t LeastName = 8;

/* How far a window's shadow darkens what lies under it, of 255. */
constexpr std::uint8_t ShadowDepth = 150;

std::int32_t DigitsOf(std::size_t Number)
{
    std::int32_t Digits = 1;
    for (; Number >= 10; Number /= 10) Digits++;
    return Digits;
}

void Shade(tui::Frame &Out, core::Point At)
{
    tui::Cell Under = Out.Read(At);
    Under.Foreground = core::Color::Mix(Under.Foreground, core::Colors::Black, ShadowDepth);
    Under.Background = core::Color::Mix(Under.Background, core::Colors::Black, ShadowDepth);
    (void)Out.Put(At, Under);
}

struct HelpLine {
    std::string      Keys;
    std::string_view Says;
    bool             Heading = false;
};

/* The help, from the table of keys: a heading for each part, a line for each
 * binding, and a binding that says nothing of its own named on the line
 * before it. */
std::vector<HelpLine> HelpLines()
{
    std::vector<HelpLine> Lines;
    const Binding        *Last = nullptr;
    std::optional<Part>   In;
    for (const Binding &One : Bindings()) {
        if (One.Says.empty()) {
            if (Last && Last->Named.empty() && Last->Does == One.Does) Lines.back().Keys += ", " + NameOf(One);
            continue;
        }
        if (In != One.In) {
            if (In) Lines.push_back({});
            Lines.push_back({std::string(PartName(One.In)), {}, true});
            In = One.In;
        }
        Lines.push_back({NameOf(One), One.Says, false});
        Last = &One;
    }
    return Lines;
}

/* The heading nearest the middle, where a second column begins; 0 when none. */
std::size_t SplitOf(const std::vector<HelpLine> &Lines)
{
    const std::size_t Half = Lines.size() / 2;
    auto Distance = [&](std::size_t Index) { return Index > Half ? Index - Half : Half - Index; };
    std::size_t Split = 0;
    for (std::size_t Index = 1; Index < Lines.size(); Index++)
        if (Lines[Index].Heading && (Split == 0 || Distance(Index) < Distance(Split))) Split = Index;
    return Split;
}

}  // namespace

/* ---- the shared ways ---------------------------------------------------- */

std::size_t CellsOf(std::string_view Text) noexcept
{
    std::size_t Cells = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte, Cells);
        Cells += Shape.Width;
        Byte += Shape.Length;
    }
    return Cells;
}

std::string Shortened(std::string_view Text, std::size_t Width)
{
    if (CellsOf(Text) <= Width) return std::string(Text);
    if (Width <= 3) return std::string(Width, '.');
    std::size_t Cells = 0, Byte = 0;
    while (Byte < Text.size()) {
        const Glyph Shape = GlyphAt(Text, Byte, Cells);
        if (Cells + Shape.Width > Width - 3) break;
        Cells += Shape.Width;
        Byte += Shape.Length;
    }
    return std::string(Text.substr(0, Byte)) + "...";
}

std::int32_t PutText(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Under)
{
    std::int32_t Col = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte, static_cast<std::size_t>(Col));
        if (Col + static_cast<std::int32_t>(Shape.Width) > Width) break;
        const bool Blank = Text[Byte] == '\t';
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++)
            (void)Out.Put({At.X + Col++, At.Y},
                          tui::Cell{Blank ? ' ' : Shape.Cells[Cell], Shape.Odd ? OddInk : Ink, Under});
        Byte += Shape.Length;
    }
    return Col;
}

core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept
{
    Width  = std::clamp(Width, 1, std::max(1, Screen.Width));
    Height = std::clamp(Height, 1, std::max(1, Screen.Height));
    return {(Screen.Width - Width) / 2, (Screen.Height - Height) / 2, Width, Height};
}

core::Rect DrawWindow(tui::Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note,
                      std::string_view Hint)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t X = Area.X + 1; X <= Right + 1; X++) Shade(Out, {X, Bottom + 1});
    for (std::int32_t Y = Area.Y + 1; Y <= Bottom; Y++) Shade(Out, {Right + 1, Y});

    Out.Fill(Area, tui::Cell{' ', WindowInk, WindowPaper});
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        (void)Out.Put({X, Area.Y}, RuleAcross, {WindowRule, WindowPaper});
        (void)Out.Put({X, Bottom}, RuleAcross, {WindowRule, WindowPaper});
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        (void)Out.Put({Area.X, Y}, RuleDown, {WindowRule, WindowPaper});
        (void)Out.Put({Right, Y}, RuleDown, {WindowRule, WindowPaper});
    }
    (void)Out.Put({Area.X, Area.Y}, CornerTL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Area.Y}, CornerTR, {WindowRule, WindowPaper});
    (void)Out.Put({Area.X, Bottom}, CornerBL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Bottom}, CornerBR, {WindowRule, WindowPaper});

    const std::int32_t Room = Area.Width - 4;
    std::int32_t       Used = 0;
    if (!Title.empty()) Used = PutText(Out, {Area.X + 2, Area.Y}, std::format(" {} ", Title), Room, TitleInk, WindowPaper);
    if (!Note.empty()) {
        const std::string  Said  = std::format(" {} ", Note);
        const std::int32_t Cells = static_cast<std::int32_t>(CellsOf(Said));
        if (Cells <= Room - Used - 1) PutText(Out, {Right - 1 - Cells, Area.Y}, Said, Cells, HintInk, WindowPaper);
    }
    if (!Hint.empty()) PutText(Out, {Area.X + 2, Bottom}, std::format(" {} ", Hint), Room, HintInk, WindowPaper);
    return {Area.X + 2, Area.Y + 1, Area.Width - 4, Area.Height - 2};
}

void DrawRule(tui::Frame &Out, core::Rect Inside, std::int32_t Row)
{
    const std::int32_t Left = Inside.X - 2, Right = Inside.X + Inside.Width + 1;
    for (std::int32_t X = Left + 1; X < Right; X++) (void)Out.Put({X, Row}, RuleAcross, {WindowRule, WindowPaper});
    (void)Out.Put({Left, Row}, TeeLeft, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Row}, TeeRight, {WindowRule, WindowPaper});
}

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
    PaintTabs(Out);
    if (!Tabs.empty()) PaintText(Out);
    PaintStatus(Out);
    switch (Now) {
    case Mode::Writing: break;
    case Mode::Picking: Chooser.Paint(Out); break;
    case Mode::Finding: Finder.Paint(Out, Active()); break;
    case Mode::GoingTo: PaintGoTo(Out); break;
    case Mode::Helping: PaintHelp(Out); break;
    case Mode::Carding: FileCard.Paint(Out, Active()); break;
    case Mode::Leaving: PaintLeaving(Out); break;
    }
    return static_cast<bool>(Screen.Present());
}

/* A tab is " N name " and " •" more while it holds changes not saved. When
 * they do not all fit, the names are shortened alike; when even the shortest
 * names do not, the tabs about the one in front are shown and arrows say
 * there are more. The whole row is the tabs': the machine's notices — the
 * clock, the pulse — are layers of their own above it, and stand over what
 * they cover. */
void Editor::PaintTabs(tui::Frame &Out) const
{
    Out.Fill({0, 0, Bounds.Width, 1}, tui::Cell{' ', BarInk, BarPaper});
    const std::int32_t Room  = Bounds.Width;
    const std::size_t  Count = Tabs.size();
    if (Count == 0 || Room <= 2) return;

    auto Fixed = [&](std::size_t Index) {
        return 3 + DigitsOf(Index + 1) + (Tabs[Index]->Dirty() ? 2 : 0);
    };
    std::int32_t Longest = 0;
    for (const std::unique_ptr<Tab> &One : Tabs) Longest = std::max(Longest, static_cast<std::int32_t>(CellsOf(One->Name)));
    auto WidthOf = [&](std::size_t Index, std::int32_t Cap) {
        return Fixed(Index) + std::min(Cap, static_cast<std::int32_t>(CellsOf(Tabs[Index]->Name)));
    };
    auto Total = [&](std::int32_t Cap) {
        std::int32_t Sum = 0;
        for (std::size_t Index = 0; Index < Count; Index++) Sum += WidthOf(Index, Cap);
        return Sum;
    };

    std::int32_t Cap = Longest;
    while (Cap > LeastName && Total(Cap) > Room) Cap--;

    /* The tab in front ends in the left half of the row: the right of it is
     * where the machine's notices stand, in layers above this one, and the
     * tab being written in is the one that must not be under them. When the
     * tabs do not all fit, or that one would end further right, the row shows
     * a stretch of them: before the front tab as many as leave it in the left
     * half, after it as many as the row takes; arrows at the ends say there
     * are more. */
    std::int32_t FrontEnds = 0;
    for (std::size_t Index = 0; Index <= Current; Index++) FrontEnds += WidthOf(Index, Cap);
    std::size_t First = 0, Last = Count;
    if (Total(Cap) > Room || FrontEnds > Room / 2) {
        First = Current;
        Last  = Current + 1;
        std::int32_t Used = 1 + WidthOf(Current, Cap);
        while (First > 0 && Used + WidthOf(First - 1, Cap) <= Room / 2) Used += WidthOf(--First, Cap);
        while (Last < Count && Used + WidthOf(Last, Cap) + 1 <= Room) Used += WidthOf(Last++, Cap);
    }

    std::int32_t X = 0;
    if (First > 0) (void)Out.Put({X++, 0}, MoreLeft, {HintInk, BarPaper});
    for (std::size_t Index = First; Index < Last; Index++) {
        const Tab        &One   = *Tabs[Index];
        const bool        Front = Index == Current;
        const core::Color Under = Front ? ShownTabPaper : BarPaper;
        const std::int32_t Wide = WidthOf(Index, Cap);
        Out.Fill({X, 0, Wide, 1}, tui::Cell{' ', BarInk, Under});

        std::int32_t Col = X + 1;
        Col += PutText(Out, {Col, 0}, std::to_string(Index + 1), Wide, One.Safety ? TroubleInk : Front ? TabNumberInk : HintInk, Under) + 1;
        Col += PutText(Out, {Col, 0}, Shortened(One.Name, static_cast<std::size_t>(Cap)), Wide, Front ? ShownTabInk : BarInk, Under);
        if (One.Dirty()) (void)Out.Put({Col + 1, 0}, tui::Glyphs::Bullet, {DirtyInk, Under});
        X += Wide;
        if (!Front && Index + 1 < Last && Index + 1 != Current) (void)Out.Put({X - 1, 0}, RuleDown, {RuleInk, BarPaper});
    }
    if (Last < Count) (void)Out.Put({X, 0}, MoreRight, {HintInk, BarPaper});
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
    const core::Color  Under   = AtCaret ? CaretLinePaper : Paper;
    const std::int32_t Gutter  = GutterWidth();
    const std::size_t  Cols    = TextCols();
    Out.Fill({Gutter, Y, Bounds.Width - Gutter, 1}, tui::Cell{' ', TextInk, Under});

    /* The numbers stand on the plain paper, the caret's one brighter: the
     * line it is on is lit only where its text is. */
    const std::string Number = std::to_string(Line + 1);
    PutText(Out, {Gutter - 3 - static_cast<std::int32_t>(Number.size()), Y}, Number, Gutter,
            AtCaret ? CaretNumberInk : NumberInk, Paper);
    (void)Out.Put({Gutter - 2, Y}, Here.Left > 0 && !Words.empty() ? MoreLeft : RuleDown, {RuleInk, Paper});

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
        const Glyph Shape = GlyphAt(Words, Byte, Column);
        const Mark *Over  = nullptr;
        for (const Mark &One : Marks)
            if (Byte >= One.From && Byte < One.To) Over = &One;
        const core::Color Ground = Selected(Byte) ? SelectedPaper : Over ? (Over->Chosen ? HerePaper : MatchPaper) : Under;
        const core::Color Ink    = Over && Over->Chosen ? HereInk : Shape.Odd ? OddInk : TextInk;
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
        (void)Out.Put({Gutter + static_cast<std::int32_t>(Cols), Y}, MoreRight, {RuleInk, Under});
}

/* Left: what the tab is and where the caret stands; right: what edit has just
 * said, or the way to the keys. */
void Editor::PaintStatus(tui::Frame &Out) const
{
    const std::int32_t Y = Bounds.Height - 1;
    Out.Fill({0, Y, Bounds.Width, 1}, tui::Cell{' ', BarInk, BarPaper});

    const std::string  Right = Message.empty() ? std::string("Ctrl+? keys") : Message;
    const core::Color  Said  = Message.empty() ? HintInk : MessageTrouble ? TroubleInk : GoodInk;
    const std::int32_t RightCells = std::min(static_cast<std::int32_t>(CellsOf(Right)), Bounds.Width - 2);
    PutText(Out, {Bounds.Width - 1 - RightCells, Y}, Shortened(Right, static_cast<std::size_t>(RightCells)), RightCells,
            Said, BarPaper);

    const std::int32_t End = Bounds.Width - 1 - RightCells - 2;
    std::int32_t       X   = 1;
    auto Put = [&](std::string_view Text, core::Color Ink, core::Color Under) {
        if (X >= End) return;
        X += PutText(Out, {X, Y}, Text, End - X, Ink, Under) + 1;
    };
    if (Tabs.empty()) {
        Put("no file open", BarInk, BarPaper);
        return;
    }

    const Tab &Here = Active();
    if (Here.Safety) Put(" SAFETY ", BadgeInk, SafetyPaper);
    if (Overtype)    Put(" OVERTYPE ", BadgeInk, OvertypePaper);
    Put(Shortened(Here.Name, 32), ShownTabInk, BarPaper);
    if (Here.File == 0) Put("(new)", HintInk, BarPaper);

    std::string Place = std::format("{}:{}", Here.Caret.Line + 1, Here.Body.ColumnOf(Here.Caret) + 1);
    if (Here.HasSelection()) {
        const auto [From, To] = Here.Selection();
        Place += From.Line == To.Line ? std::format("  {} bytes selected", To.Byte - From.Byte)
                                      : std::format("  {} lines selected", To.Line - From.Line + 1);
    }
    if (Here.Body.Crlf()) Place += "  CRLF";
    Put(Place, BarInk, BarPaper);

    const std::vector<std::string> Worn = ShownTags(Here.Tags, Here.Name);
    if (!Worn.empty()) Put(Shortened("[" + Joined(Worn, ", ") + "]", static_cast<std::size_t>(std::max(0, End - X))),
                           HintInk, BarPaper);
}

void Editor::PaintGoTo(tui::Frame &Out) const
{
    const std::int32_t Width  = std::min(Bounds.Width - 2, 40);
    const core::Rect   Inside = DrawWindow(Out, {Bounds.Width - Width - 1, 1, Width, 3}, "Go to line",
                                           std::format("of {}", Active().Body.LineCount()), "line or line:column");
    LineField.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, true);
}

/* Two columns side by side where the glass is wide enough, split between two
 * parts; one column that scrolls where it is not. */
void Editor::PaintHelp(tui::Frame &Out) const
{
    const std::vector<HelpLine> Lines = HelpLines();
    std::size_t                 KeyCells = 0, SayCells = 0;
    for (const HelpLine &One : Lines) {
        if (One.Heading) continue;
        KeyCells = std::max(KeyCells, CellsOf(One.Keys));
        SayCells = std::max(SayCells, CellsOf(One.Says));
    }
    const auto LineCells = static_cast<std::int32_t>(KeyCells + 2 + SayCells);

    /* The blank line before the heading the second column begins with stays
     * behind at the foot of the first. */
    const std::size_t  Split = SplitOf(Lines);
    const bool         Two   = Split != 0 && 2 * LineCells + 3 <= Bounds.Width - 6;
    const std::size_t  Tall  = Two ? std::max(Split - 1, Lines.size() - Split) : Lines.size();
    const std::int32_t Width = Two ? 2 * LineCells + 3 + 4 : LineCells + 4;
    const core::Rect  Area   = Centred(Bounds, Width, static_cast<std::int32_t>(Tall) + 2);
    const auto        Rows   = static_cast<std::size_t>(Area.Height - 2);
    const std::size_t From   = std::min(HelpFirst, Tall > Rows ? Tall - Rows : 0);
    const core::Rect  Inside = DrawWindow(Out, Area, "Keys", "edit",
                                          Tall > Rows ? "Up/Down scroll  any other key closes" : "any key closes");

    auto PaintColumn = [&](std::size_t Begin, std::size_t End, std::int32_t X) {
        for (std::size_t Row = 0; Row < Rows && Begin + From + Row < End; Row++) {
            const HelpLine    &One = Lines[Begin + From + Row];
            const std::int32_t Y   = Inside.Y + static_cast<std::int32_t>(Row);
            if (One.Heading) {
                PutText(Out, {X, Y}, One.Keys, LineCells, TabNumberInk, WindowPaper);
                continue;
            }
            PutText(Out, {X, Y}, One.Keys, static_cast<std::int32_t>(KeyCells), TitleInk, WindowPaper);
            PutText(Out, {X + static_cast<std::int32_t>(KeyCells) + 2, Y}, One.Says, static_cast<std::int32_t>(SayCells),
                    WindowInk, WindowPaper);
        }
    };
    PaintColumn(0, Two ? Split - 1 : Lines.size(), Inside.X);
    if (Two) PaintColumn(Split, Lines.size(), Inside.X + LineCells + 3);
}

void Editor::PaintLeaving(tui::Frame &Out) const
{
    const std::size_t Room   = Bounds.Height > 6 ? static_cast<std::size_t>(Bounds.Height - 6) : 1;
    const std::size_t Listed = std::min(Unsaved.size(), Room);
    const bool        More   = Listed < Unsaved.size();
    const core::Rect  Area   = Centred(Bounds, std::min(Bounds.Width - 4, 60),
                                       static_cast<std::int32_t>(Listed + (More ? 1 : 0)) + 2);
    const core::Rect  Inside = DrawWindow(Out, Area, "Changes not saved", "",
                                          LeavingAll ? "S save all  D leave without saving  Esc stay"
                                                     : "S save  D close without saving  Esc keep it");
    for (std::size_t Index = 0; Index < Listed; Index++) {
        const Tab &One = *Unsaved[Index];
        PutText(Out, {Inside.X, Inside.Y + static_cast<std::int32_t>(Index)},
                One.File == 0 ? One.Name + "  (new)" : One.Name, Inside.Width, TitleInk, WindowPaper);
    }
    if (More)
        PutText(Out, {Inside.X, Inside.Y + static_cast<std::int32_t>(Listed)},
                std::format("and {} more", Unsaved.size() - Listed), Inside.Width, HintInk, WindowPaper);
}

}  // namespace edit
