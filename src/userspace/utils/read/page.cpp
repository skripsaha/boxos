/* read's picture: the tabs, the page, the status line, a window in front. */

#include <algorithm>
#include <format>

#include "painter/tui/glyph.h"

#include "kin/glyph.h"

#include "reader.h"

namespace read {

namespace {

/* What a page holds beyond plain words, in colours of its own; the paper, the
 * ink, the bars and the windows are the family's (kin/look.h). */
constexpr core::Color HeadingInk    = core::Color::Rgb(0xF2, 0xC4, 0x6F);
constexpr core::Color SubheadingInk = core::Color::Rgb(0xC9, 0xA8, 0x6A);
constexpr core::Color StrongInk     = core::Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr core::Color EmphasisInk   = core::Color::Rgb(0xA8, 0xC7, 0xE8);
constexpr core::Color CodeInk       = core::Color::Rgb(0xB8, 0xD4, 0xA0);
constexpr core::Color CodePaper     = core::Color::Rgb(0x22, 0x22, 0x22);
constexpr core::Color LinkInk       = core::Color::Rgb(0x7F, 0xB8, 0xE6);
constexpr core::Color AccentInk     = core::Color::Rgb(0xF2, 0xC4, 0x6F);   /* an item's marker, a bookmark's sign */
constexpr core::Color WayPaper      = core::Color::Rgb(0x22, 0x55, 0x88);   /* the badge of the other way of reading */

constexpr char BookmarkSign = static_cast<char>(0x10);   /* ► */

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
    const Tab  &Here = Active();
    std::string Found;
    if (Here.Lines.Rows().empty()) return Found;
    const std::size_t Block = Here.BlockOfReading();
    for (const std::size_t One : Here.Body.Headings()) {
        if (One > Block) break;
        Found = Here.Body.Blocks[One].Text;
    }
    return Found;
}

bool Reader::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({TextInk, Paper});
    std::vector<TabFace> Faces;
    for (const std::unique_ptr<Tab> &One : Tabs) Faces.push_back({One->Body.Name});
    PaintTabs(Out, Bounds.Width, Faces, Current);

    if (!Tabs.empty()) {
        const Tab &Here = Active();
        if (Here.Lines.Rows().empty()) {
            const std::string_view Empty = "there is nothing in this file to read";
            PutText(Out, {(Bounds.Width - static_cast<std::int32_t>(Empty.size())) / 2, Bounds.Height / 2}, Empty,
                    Bounds.Width, HintInk, Paper);
        }
        for (std::size_t Line = 0; Line < PageRows() && Here.Top + Line < Here.Lines.Rows().size(); Line++)
            PaintRow(Out, 1 + static_cast<std::int32_t>(Line), Here.Top + Line);
    }
    PaintStatus(Out);

    switch (Now) {
    case Mode::Reading: break;
    case Mode::Picking: Chooser.Paint(Out); break;
    case Mode::Finding: Finder.Paint(Out); break;
    case Mode::Listing: Shown.Paint(Out); break;
    case Mode::Helping: Keys.Paint(Out, Bounds); break;
    case Mode::Carding: FileCard.Paint(Out); break;
    }
    return static_cast<bool>(Screen.Present());
}

void Reader::PaintRow(tui::Frame &Out, std::int32_t Y, std::size_t Index) const
{
    const Tab         &Here   = Active();
    const Row         &One    = Here.Lines.Rows()[Index];
    const std::int32_t X      = Left();
    const std::int32_t Cols   = Column();
    const auto [From, To]     = Here.Selection();
    const bool         Chosen = Here.Anchor && Index >= From && Index <= To;
    const core::Color  Under  = Chosen ? SelectedPaper : Index == Here.Reading ? LinePaper : Paper;
    if (Under != Paper) Out.Fill({X - 1, Y, Cols + 2, 1}, tui::Cell{' ', TextInk, Under});

    switch (One.Is) {
    case RowKind::Space: return;
    case RowKind::RuleLine: Out.Fill({X, Y, Cols, 1}, tui::Cell{RuleAcross, HintInk, Under}); return;
    case RowKind::Underline: {
        const Block &Heading = Here.Body.Blocks[One.Block];
        std::size_t  Longest = 0;
        for (std::size_t Row = Index; Row > 0 && Here.Lines.Rows()[Row - 1].Block == One.Block; Row--) {
            const read::Row &Above = Here.Lines.Rows()[Row - 1];
            Longest = std::max(Longest, CellsOf(std::string_view(Heading.Text).substr(Above.From, Above.To - Above.From)));
        }
        Out.Fill({X, Y, std::min(static_cast<std::int32_t>(Longest), Cols), 1},
                 tui::Cell{Heading.Level == 1 ? DoubleRule : RuleAcross, HeadingInk, Under});
        return;
    }
    case RowKind::Text: break;
    }

    if (One.First)
        for (const Mark &Put : Here.Marked)
            if (Here.Body.BlockAt(Put.Source) == One.Block)
                (void)Out.Put({X - 2, Y}, BookmarkSign, {Put.Lost ? TroubleInk : AccentInk, Paper});
    PaintText(Out, Y, Index, Under);
}

/* A row's words: an item's marker or a quote's bar before them, each stretch
 * in its look, what the find window found lit while it is open. Code and
 * tables are cut at the column's edge, and » says the line goes on. */
void Reader::PaintText(tui::Frame &Out, std::int32_t Y, std::size_t Index, core::Color Under) const
{
    const Tab         &Here  = Active();
    const Row         &One   = Here.Lines.Rows()[Index];
    const Block       &Of    = Here.Body.Blocks[One.Block];
    const std::int32_t X     = Left();
    const std::int32_t Cols  = Column();
    const bool         Code  = Of.Is == Kind::Code;
    const bool         Kept  = Code || Of.Is == Kind::Table;
    const core::Color  Ground = Code && Under == Paper ? CodePaper : Under;
    if (Code) Out.Fill({X, Y, Cols, 1}, tui::Cell{' ', CodeInk, Ground});

    if (Of.Is == Kind::Item && One.First) {
        const std::int32_t At = X + Layout::MarkerAt(Of);
        if (Of.Marker.empty()) (void)Out.Put({At, Y}, tui::Glyphs::Bullet, {AccentInk, Under});
        else                   PutText(Out, {At, Y}, Of.Marker, Cols, AccentInk, Under);
    }
    if (Of.Is == Kind::Quote) (void)Out.Put({X + Layout::MarkerAt(Of), Y}, RuleDown, {HintInk, Under});

    core::Color Base = TextInk;
    if (Of.Is == Kind::Heading)    Base = Of.Level <= 2 ? HeadingInk : SubheadingInk;
    else if (Of.Is == Kind::Quote) Base = HintInk;
    else if (Code)                 Base = CodeInk;

    /* The places found in this block, found by halves: they are in the
     * book's order. */
    const std::vector<Search::Match> &Found  = Finder.Matches();
    const bool                        Lit    = Now == Mode::Finding;
    const auto                        Begin  = std::lower_bound(Found.begin(), Found.end(), One.Block,
                                                                [](const Search::Match &Match, std::size_t Block) { return Match.Block < Block; });
    const std::optional<std::size_t>  AtHere = Finder.Here();

    const std::string_view Text  = Of.Text;
    const std::int32_t     End   = X + Cols;
    const bool             Cut   = Kept && static_cast<std::int32_t>(CellsOf(Text.substr(One.From, One.To - One.From))) > Cols - One.Indent;
    const std::int32_t     Limit = Cut ? End - 1 : End;
    std::int32_t           Col   = X + One.Indent;
    for (std::size_t Byte = One.From; Byte < One.To;) {
        const Glyph Shape = GlyphAt(Text, Byte);
        if (Col + static_cast<std::int32_t>(Shape.Width) > Limit) break;

        core::Color Ink = Base, Behind = Ground;
        for (const read::Run &Stretch : Of.Looks) {
            if (Byte < Stretch.From || Byte >= Stretch.To) continue;
            switch (Stretch.Is) {
            case read::Look::Strong:   Ink = StrongInk; break;
            case read::Look::Emphasis: Ink = EmphasisInk; break;
            case read::Look::Link:     Ink = LinkInk; break;
            case read::Look::Code:
                Ink = CodeInk;
                if (Behind == Paper) Behind = CodePaper;
                break;
            case read::Look::Plain: break;
            }
            break;
        }
        for (auto Match = Begin; Lit && Match != Found.end() && Match->Block == One.Block; ++Match) {
            if (Byte < Match->From || Byte >= Match->To) continue;
            const bool IsHere = AtHere && static_cast<std::size_t>(Match - Found.begin()) == *AtHere;
            Behind            = IsHere ? HerePaper : MatchPaper;
            if (IsHere) Ink = HereInk;
            break;
        }
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++)
            (void)Out.Put({Col++, Y}, tui::Cell{Shape.Cells[Cell], Shape.Odd ? OddInk : Ink, Behind});
        Byte += Shape.Length;
    }
    if (Cut) (void)Out.Put({End - 1, Y}, MoreRight, {HintInk, Ground});
}

/* Left: what the book is, and how far through it the reading line is — the
 * page, how long the rest takes, the heading it is under; right: what read
 * has just said, or the way to the keys. */
void Reader::PaintStatus(tui::Frame &Out) const
{
    StatusLine Line(Out, Bounds, Message, MessageTrouble);
    if (Tabs.empty()) {
        Line.Put("no file open", BarInk);
        return;
    }

    const Tab &Here = Active();
    if (Looking == Scope::Everywhere) Line.Badge("EVERYWHERE", EverywherePaper);
    if (Here.Body.As != ReadingFor(Here.Body.Name))
        Line.Badge(Here.Body.As == Reading::Lines ? "LINES" : "PROSE", WayPaper);
    Line.Put(Shortened(Here.Body.Name, 32), ShownTabInk);

    const std::size_t Rows = Here.Lines.Rows().size();
    if (Rows == 0) return;
    const std::size_t Page    = Here.Top >= Here.MostTop(PageRows()) ? Here.Pages(PageRows()) : Here.PageOf(Here.Top, PageRows());
    const std::size_t Percent = Rows <= 1 ? 100 : Here.Reading * 100 / (Rows - 1);
    std::string       Place   = std::format("page {} of {}  {}%", Page, Here.Pages(PageRows()), Percent);
    if (Here.HasSelection()) {
        const auto [From, To] = Here.Selection();
        std::size_t Count     = 0;
        for (std::size_t Row = From; Row <= To; Row++) Count += Here.TextRow(Row) ? 1 : 0;
        Place += std::format("  {} line{} selected", Count, Count == 1 ? "" : "s");
    }
    Line.Put(Place, BarInk);

    const std::size_t Minutes = (Here.Body.WordsFrom(Here.BlockOfReading()) + WordsAMinute - 1) / WordsAMinute;
    if (Minutes == 0)      Line.Put("the end", HintInk);
    else if (Minutes < 60) Line.Put(std::format("≈ {} min left", Minutes), HintInk);
    else                   Line.Put(std::format("≈ {} h {} min left", Minutes / 60, Minutes % 60), HintInk);

    const std::string Under = HeadingAbove();
    if (!Under.empty()) Line.Put(Shortened(Under, static_cast<std::size_t>(Line.Room())), HintInk);
}

}  // namespace read
