/* hud.cpp — the words and lines round the picture (hud.h). */

#include "hud.h"

#include <algorithm>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;
namespace G = painter::tui::Glyphs;

constexpr char32_t DoubleDown = U'\u2551';   /* ║ */
constexpr char32_t DoubleTL = U'\u2554';     /* ╔ */
constexpr char32_t DoubleTR = U'\u2557';     /* ╗ */
constexpr char32_t DoubleBL = U'\u255A';     /* ╚ */
constexpr char32_t DoubleBR = U'\u255D';     /* ╝ */

std::int32_t Cells(std::string_view Text) { return static_cast<std::int32_t>(tui::CellsOf(Text)); }

// A frame of these glyphs round Area, and what each edge is made of.
struct RuleSet {
    char32_t Across, Down, TopLeft, TopRight, BottomLeft, BottomRight;
};

void Edge(tui::Frame &Out, core::Rect Area, const RuleSet &Of, tui::TextStyle Style)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t Column = Area.X + 1; Column < Right; Column++) {
        Out.Put({Column, Area.Y}, Of.Across, Style);
        Out.Put({Column, Bottom}, Of.Across, Style);
    }
    for (std::int32_t Row = Area.Y + 1; Row < Bottom; Row++) {
        Out.Put({Area.X, Row}, Of.Down, Style);
        Out.Put({Right, Row}, Of.Down, Style);
    }
    Out.Put({Area.X, Area.Y}, Of.TopLeft, Style);
    Out.Put({Right, Area.Y}, Of.TopRight, Style);
    Out.Put({Area.X, Bottom}, Of.BottomLeft, Style);
    Out.Put({Right, Bottom}, Of.BottomRight, Style);
}

void Shade(tui::Frame &Out, core::Point At)
{
    if (At.X < 0 || At.Y < 0 || At.X >= Out.Size().Width || At.Y >= Out.Size().Height) return;
    tui::Cell Shown = Out.Read(At);
    Shown.Foreground = Darker(Shown.Foreground, 0.55f);
    Shown.Background = Darker(Shown.Background, 0.55f);
    Out.Put(At, Shown);
}

}  // namespace

std::vector<std::string> Wrapped(std::string_view Text, std::size_t Width)
{
    std::vector<std::string> Lines;
    std::string              Line;
    std::size_t              At = 0;
    while (At < Text.size()) {
        std::size_t Stop = Text.find(' ', At);
        if (Stop == std::string_view::npos) Stop = Text.size();
        const std::string_view Word = Text.substr(At, Stop - At);
        if (!Line.empty() && tui::CellsOf(Line) + 1 + tui::CellsOf(Word) > Width) {
            Lines.push_back(Line);
            Line.clear();
        }
        Line += (Line.empty() ? "" : " ") + std::string(Word);
        At = Stop + 1;
    }
    if (!Line.empty()) Lines.push_back(Line);
    return Lines;
}

void PaintBar(tui::Frame &Out, const Look &Shown, std::string_view Left, std::string_view Note)
{
    const core::Size Size = Out.Size();
    Out.Fill(core::Rect{0, 0, Size.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    const std::int32_t Room = std::max(8, Size.Width - OverlayCols);
    const std::int32_t Used = 1 + tui::PutText(Out, core::Point{1, 0}, Left, Room - 2, Shown.Title, Shown.Bars);
    if (!Note.empty()) tui::PutText(Out, core::Point{Used + 2, 0}, Note, std::max(0, Room - Used - 3), Shown.Quiet, Shown.Bars);
}

void PaintHints(tui::Frame &Out, const Look &Shown, const std::vector<Hint> &Hints, std::string_view Right)
{
    const core::Size   Size = Out.Size();
    const std::int32_t Row = Size.Height - 1;
    Out.Fill(core::Rect{0, Row, Size.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    std::int32_t Budget = Size.Width - 2 - (Right.empty() ? 0 : Cells(Right) + 3);
    std::vector<bool> Fits(Hints.size(), false);
    int               Last = 0;
    for (const Hint &One : Hints) Last = std::max(Last, One.Rank);
    for (int Rank = 0; Rank <= Last; Rank++) {
        for (std::size_t At = 0; At < Hints.size(); At++) {
            if (Hints[At].Rank != Rank) continue;
            const std::int32_t Wide = Cells(Hints[At].Key) + 1 + Cells(Hints[At].Does) + 2;
            if (Wide <= Budget) {
                Fits[At] = true;
                Budget -= Wide;
            }
        }
    }
    std::int32_t Column = 1;
    for (std::size_t At = 0; At < Hints.size(); At++) {
        if (!Fits[At]) continue;
        Column += tui::PutText(Out, core::Point{Column, Row}, Hints[At].Key, Size.Width - Column, Hints[At].Urgent ? Shown.Fire : Shown.Title, Shown.Bars);
        Column += 1 + tui::PutText(Out, core::Point{Column + 1, Row}, Hints[At].Does, Size.Width - Column - 1, Shown.Quiet, Shown.Bars) + 2;
    }
    if (!Right.empty()) tui::PutText(Out, core::Point{Size.Width - Cells(Right) - 1, Row}, Right, Cells(Right), Shown.Title, Shown.Bars);
}

void PutCentred(tui::Frame &Out, std::int32_t Left, std::int32_t Width, std::int32_t Row, std::string_view Text, Color Ink, Color Under)
{
    const std::int32_t Wide = std::min(Width, Cells(Text));
    tui::PutText(Out, core::Point{Left + (Width - Wide) / 2, Row}, Text, Wide, Ink, Under);
}

void PaintSaid(tui::Frame &Out, std::int32_t Row, std::string_view Text, Color Ink, Color Under)
{
    Out.Fill(core::Rect{0, Row, Out.Size().Width, 1}, ' ', {Ink, Under});
    PutCentred(Out, 0, Out.Size().Width, Row, Text, Ink, Under);
}

void PaintFrame(tui::Frame &Out, core::Rect Cells, Color Rule, Color Under)
{
    Edge(Out, Cells, RuleSet{G::RuleAcross, G::RuleDown, G::CornerTL, G::CornerTR, G::CornerBL, G::CornerBR}, {Rule, Under});
}

core::Rect PaintPanel(tui::Frame &Out, const Look &Shown, core::Rect Area, std::string_view Title, std::string_view Foot)
{
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    for (std::int32_t Row = Area.Y + 1; Row <= Area.Y + Area.Height; Row++) Shade(Out, {Area.X + Area.Width, Row});
    for (std::int32_t Column = Area.X + 1; Column <= Area.X + Area.Width; Column++) Shade(Out, {Column, Area.Y + Area.Height});
    Out.Fill(Area, ' ', {Shown.Text, Paper});
    Edge(Out, Area, RuleSet{G::DoubleRule, DoubleDown, DoubleTL, DoubleTR, DoubleBL, DoubleBR}, {Shown.Title, Paper});
    if (!Title.empty()) {
        const std::int32_t Wide = std::min(Cells(Title), Area.Width - 8);
        tui::PutText(Out, core::Point{Area.X + 2, Area.Y}, " ", 1, Shown.Title, Paper);
        tui::PutText(Out, core::Point{Area.X + 3, Area.Y}, Title, Wide, Shown.Title, Paper);
        tui::PutText(Out, core::Point{Area.X + 3 + Wide, Area.Y}, " ", 1, Shown.Title, Paper);
    }
    if (!Foot.empty()) {
        const std::int32_t Wide = std::min(Cells(Foot), Area.Width - 8);
        tui::PutText(Out, core::Point{Area.X + 2, Area.Y + Area.Height - 1}, " ", 1, Shown.Title, Paper);
        tui::PutText(Out, core::Point{Area.X + 3, Area.Y + Area.Height - 1}, Foot, Wide, Shown.Quiet, Paper);
        tui::PutText(Out, core::Point{Area.X + 3 + Wide, Area.Y + Area.Height - 1}, " ", 1, Shown.Title, Paper);
    }
    return core::Rect{Area.X + 2, Area.Y + 1, Area.Width - 4, Area.Height - 2};
}

void Darken(tui::Frame &Out, int Share)
{
    const core::Size Size = Out.Size();
    for (std::int32_t Row = 0; Row < Size.Height; Row++) {
        for (std::int32_t Column = 0; Column < Size.Width; Column++) {
            tui::Cell Shown = Out.Read({Column, Row});
            Shown.Foreground = Mixed(Color::Rgb(0, 0, 0), Shown.Foreground, Share);
            Shown.Background = Mixed(Color::Rgb(0, 0, 0), Shown.Background, Share);
            Out.Put({Column, Row}, Shown);
        }
    }
}

}  // namespace seabattle
