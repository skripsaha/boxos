/* read's picture: the title row, the page, the status row, a window in front. */

#include <algorithm>
#include <format>

#include "painter/tui/glyph.h"

#include "glyph.h"
#include "reader.h"

namespace read {

namespace {

/* What a reader takes in, in words a minute: the time left is told by it. */
constexpr std::size_t WordsAMinute = 230;

}  // namespace

std::int32_t Reader::Column() const noexcept
{
    const std::int32_t Most = std::max(8, Bounds.Width - 4);
    return std::clamp(Width != 0 ? Width : UsualWidth, std::min(LeastWidth, Most), Most);
}

std::int32_t Reader::Left() const noexcept
{
    return (Bounds.Width - Column()) / 2;
}

std::size_t Reader::PageRows() const noexcept
{
    return Bounds.Height > 2 ? static_cast<std::size_t>(Bounds.Height - 2) : 1;
}

/* The heading the reading line is under. */
std::string Reader::HeadingAbove() const
{
    if (Lines.Rows().empty()) return {};
    const std::size_t Block = Lines.Rows()[Reading].Block;
    std::string       Found;
    for (const std::size_t One : Here.Headings()) {
        if (One > Block) break;
        Found = Here.Blocks[One].Text;
    }
    return Found;
}

/* How far through, which page, and the time the rest of it takes. */
std::string Reader::Progress() const
{
    const std::size_t Rows = Lines.Rows().size();
    if (Rows == 0) return {};
    const std::size_t Percent = Rows <= 1 ? 100 : Reading * 100 / (Rows - 1);
    const std::size_t Page    = Top >= MostTop() ? Pages() : PageOf(Top);
    const std::size_t Minutes = (Here.WordsFrom(Lines.Rows()[Reading].Block) + WordsAMinute - 1) / WordsAMinute;

    std::string Left;
    if (Minutes == 0)       Left = "the end";
    else if (Minutes < 60)  Left = std::format("≈ {} min", Minutes);
    else                    Left = std::format("≈ {} h {} min", Minutes / 60, Minutes % 60);
    return std::format("{}%  page {} of {}  {}", Percent, Page, Pages(), Left);
}

bool Reader::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({Scheme->Ink, Scheme->Paper});
    PaintTitle(Out);

    if (Lines.Rows().empty()) {
        const std::string_view Empty = "there is nothing in this file to read";
        PutText(Out, {(Bounds.Width - static_cast<std::int32_t>(Empty.size())) / 2, Bounds.Height / 2}, Empty,
                Bounds.Width, Scheme->Faint, Scheme->Paper);
    }
    for (std::size_t Line = 0; Line < PageRows() && Top + Line < Lines.Rows().size(); Line++)
        PaintRow(Out, 1 + static_cast<std::int32_t>(Line), Top + Line);

    PaintStatus(Out);
    if (Now == Mode::Listing) Shown.Paint(Out, *Scheme);
    return static_cast<bool>(Screen.Present());
}

/* The book's name, which of the books it is, and the heading the reading line
 * is under. The machine's own notices stand over the right of it. */
void Reader::PaintTitle(tui::Frame &Out) const
{
    Out.Fill({0, 0, Bounds.Width, 1}, tui::Cell{' ', Scheme->BarInk, Scheme->BarPaper});
    std::int32_t Col = 1;
    Col += PutText(Out, {Col, 0}, Here.Name, Bounds.Width - Col, Scheme->Ink, Scheme->BarPaper);
    if (Files.size() > 1)
        Col += PutText(Out, {Col, 0}, std::format("  {} of {}", Current + 1, Files.size()), Bounds.Width - Col,
                       Scheme->Faint, Scheme->BarPaper);
    const std::string Under = HeadingAbove();
    if (!Under.empty()) {
        Col += PutText(Out, {Col, 0}, "  —  ", Bounds.Width - Col, Scheme->Faint, Scheme->BarPaper);
        PutText(Out, {Col, 0}, Under, Bounds.Width - Col, Scheme->Heading, Scheme->BarPaper);
    }
}

void Reader::PaintRow(tui::Frame &Out, std::int32_t Y, std::size_t Index) const
{
    const Row         &One     = Lines.Rows()[Index];
    const std::int32_t X       = Left();
    const std::int32_t Cols    = Column();
    const bool         Chosen  = Anchor && Index >= std::min(*Anchor, Reading) && Index <= std::max(*Anchor, Reading);
    const core::Color  Paper   = Chosen ? Scheme->SelectedPaper : Index == Reading ? Scheme->ReadingPaper : Scheme->Paper;
    if (Paper != Scheme->Paper) Out.Fill({X - 1, Y, Cols + 2, 1}, tui::Cell{' ', Scheme->Ink, Paper});

    switch (One.Is) {
    case RowKind::Space: return;
    case RowKind::RuleLine: Out.Fill({X, Y, Cols, 1}, tui::Cell{RuleAcross, Scheme->Faint, Paper}); return;
    case RowKind::Underline: {
        const Block &Heading = Here.Blocks[One.Block];
        std::size_t  Longest = 0;
        for (std::size_t Row = Index; Row > 0 && Lines.Rows()[Row - 1].Block == One.Block; Row--) {
            const read::Row &Above = Lines.Rows()[Row - 1];
            Longest = std::max(Longest, CellsOf(std::string_view(Heading.Text).substr(Above.From, Above.To - Above.From)));
        }
        Out.Fill({X, Y, std::min(static_cast<std::int32_t>(Longest), Cols), 1},
                 tui::Cell{Heading.Level == 1 ? DoubleRule : RuleAcross, Scheme->Heading, Paper});
        return;
    }
    case RowKind::Text: break;
    }

    if (One.First && std::find(Marked.begin(), Marked.end(), Here.Blocks[One.Block].Source) != Marked.end())
        (void)Out.Put({X - 2, Y}, BookmarkSign, {Scheme->Accent, Scheme->Paper});
    PaintText(Out, Y, Index, Paper);
}

/* A row's words: an item's marker or a quote's bar before them, each stretch
 * in its look, what the search found lit. Code and tables are cut at the
 * column's edge, and » says the line goes on. */
void Reader::PaintText(tui::Frame &Out, std::int32_t Y, std::size_t Index, core::Color Paper) const
{
    const Row         &One   = Lines.Rows()[Index];
    const Block       &Of    = Here.Blocks[One.Block];
    const std::int32_t X     = Left();
    const std::int32_t Cols  = Column();
    const bool         Code  = Of.Is == Kind::Code;
    const bool         Kept  = Code || Of.Is == Kind::Table;
    const core::Color  Under = Code && Paper == Scheme->Paper ? Scheme->CodePaper : Paper;
    if (Code) Out.Fill({X, Y, Cols, 1}, tui::Cell{' ', Scheme->Code, Under});

    if (Of.Is == Kind::Item && One.First) {
        const std::int32_t At = X + Layout::MarkerAt(Of);
        if (Of.Marker.empty()) (void)Out.Put({At, Y}, tui::Glyphs::Bullet, {Scheme->Accent, Paper});
        else                   PutText(Out, {At, Y}, Of.Marker, Cols, Scheme->Accent, Paper);
    }
    if (Of.Is == Kind::Quote) (void)Out.Put({X + Layout::MarkerAt(Of), Y}, RuleDown, {Scheme->Faint, Paper});

    core::Color Base = Scheme->Ink;
    if (Of.Is == Kind::Heading)    Base = Of.Level <= 2 ? Scheme->Heading : Scheme->Subheading;
    else if (Of.Is == Kind::Quote) Base = Scheme->Faint;
    else if (Code)                 Base = Scheme->Code;

    /* The matches in this block, found by halves: they are in the book's order. */
    const auto Begin = std::lower_bound(Matches.begin(), Matches.end(), One.Block,
                                        [](const Match &Found, std::size_t Block) { return Found.Block < Block; });

    const std::string_view Text  = Of.Text;
    const std::int32_t     End   = X + Cols;
    const bool             Cut   = Kept && static_cast<std::int32_t>(CellsOf(Text.substr(One.From, One.To - One.From))) > Cols - One.Indent;
    const std::int32_t     Limit = Cut ? End - 1 : End;
    std::int32_t           Col   = X + One.Indent;
    for (std::size_t Byte = One.From; Byte < One.To;) {
        const Glyph Shape = GlyphAt(Text, Byte);
        if (Col + static_cast<std::int32_t>(Shape.Width) > Limit) break;

        core::Color Ink = Base, Behind = Under;
        for (const read::Run &Stretch : Of.Looks) {
            if (Byte < Stretch.From || Byte >= Stretch.To) continue;
            switch (Stretch.Is) {
            case read::Look::Strong:   Ink = Scheme->Strong; break;
            case read::Look::Emphasis: Ink = Scheme->Emphasis; break;
            case read::Look::Link:     Ink = Scheme->Link; break;
            case read::Look::Code:
                Ink = Scheme->Code;
                if (Behind == Scheme->Paper) Behind = Scheme->CodePaper;
                break;
            case read::Look::Plain: break;
            }
            break;
        }
        for (auto Found = Begin; Found != Matches.end() && Found->Block == One.Block; ++Found) {
            if (Byte < Found->From || Byte >= Found->To) continue;
            const bool IsHere = static_cast<std::size_t>(Found - Matches.begin()) == AtMatch;
            Behind           = IsHere ? Scheme->HerePaper : Scheme->MatchPaper;
            if (IsHere) Ink = Scheme->HereInk;
            break;
        }
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++)
            (void)Out.Put({Col++, Y}, tui::Cell{Shape.Cells[Cell], Ink, Behind});
        Byte += Shape.Length;
    }
    if (Cut) (void)Out.Put({End - 1, Y}, GoesOn, {Scheme->Faint, Under});
}

/* What was said, or the found and the keys; how far through, on the right. A
 * search is typed here, its caret in the frame. */
void Reader::PaintStatus(tui::Frame &Out) const
{
    const std::int32_t Y = Bounds.Height - 1;
    Out.Fill({0, Y, Bounds.Width, 1}, tui::Cell{' ', Scheme->BarInk, Scheme->BarPaper});

    const std::string  Right      = Progress();
    const std::int32_t RightCells = static_cast<std::int32_t>(CellsOf(Right));
    PutText(Out, {Bounds.Width - RightCells - 1, Y}, Right, RightCells, Scheme->BarInk, Scheme->BarPaper);
    const std::int32_t Room = Bounds.Width - RightCells - 3;

    if (Now == Mode::Finding) {
        const std::int32_t Said = PutText(Out, {1, Y}, "find ", Room, Scheme->Faint, Scheme->BarPaper);
        const std::int32_t Typed = PutText(Out, {1 + Said, Y}, Needle, Room - Said, Scheme->Ink, Scheme->BarPaper);
        if (!Needle.empty()) {
            const std::string Count = Matches.empty() ? std::string("  nothing found")
                                                      : std::format("  {} of {}", AtMatch + 1, Matches.size());
            PutText(Out, {1 + Said + Typed + 1, Y}, Count, Room - Said - Typed - 1, Scheme->Faint, Scheme->BarPaper);
        }
        (void)Out.SetCaret(core::Point{1 + Said + Typed, Y});
        return;
    }
    if (!Message.empty()) {
        PutText(Out, {1, Y}, Shortened(Message, static_cast<std::size_t>(std::max(Room, 1))), Room,
                MessageTrouble ? Scheme->Trouble : Scheme->BarInk, Scheme->BarPaper);
        return;
    }
    const std::string Hint = Matches.empty() ? std::string("? keys")
                                             : std::format("{} of {} found   n next   N back", AtMatch + 1, Matches.size());
    PutText(Out, {1, Y}, Hint, Room, Scheme->Faint, Scheme->BarPaper);
}

}  // namespace read
