/* typer_paint.cpp — the screen while typing: a title, a bar for how far the
 * test has come, the passage in the middle with room on both sides, and a foot
 * with the live numbers. A mistake is drawn where it was made, in red, showing
 * the character that was wanted. */

#include <algorithm>
#include <string>

#include "typer_glass.h"

namespace {

/* Projected on a text console: RightInk white (a grey of L217), WrongPaper dark
 * red, RestInk and BarEmpty dark grey (L110, L48), CaretPaper light yellow,
 * BarFilled dark cyan, and the bar fades through to AmberInk's yellow. */
constexpr PainterColor RightInk   = PainterColor::Rgb(0xC8, 0xEB, 0xC8);
constexpr PainterColor WrongPaper = PainterColor::Rgb(0xB0, 0x28, 0x30);
constexpr PainterColor RestInk    = PainterColor::Rgb(0x6E, 0x6E, 0x6E);
constexpr PainterColor CaretPaper = PainterColor::Rgb(0xF0, 0xC8, 0x60);
constexpr PainterColor BarFilled  = PainterColor::Rgb(0x30, 0xB8, 0x98);
constexpr PainterColor BarEmpty   = PainterColor::Rgb(0x30, 0x30, 0x30);

constexpr std::uint32_t BarRow      = 2;
constexpr std::uint32_t TextRow     = 4;
constexpr std::uint32_t BelowText   = 4;
constexpr std::uint32_t MarginLeast = 8;
constexpr std::uint32_t WidthMost   = 72;
constexpr char          FullBlock   = static_cast<char>(0xDB);
constexpr char          LightShade  = static_cast<char>(0xB0);

std::string KindWords(const Task &Job)
{
    std::string Words = Job.Kind == TextKind::Code ? "code" : Job.Kind == TextKind::File ? "your file" : "everyday prose";
    if (Job.Seconds != 0) Words += "  -  " + std::to_string(Job.Seconds) + " second test";
    return Words;
}

void PaintTitle(Screen &Glass, const Task &Job)
{
    Glass.Fill(0, 0, Glass.Cols, ' ', TitleInk, BarPaper);
    std::uint32_t Col = Glass.Write(0, 2, "typer", AmberInk, BarPaper);
    Glass.Write(0, Col + 3, KindWords(Job), TitleInk, BarPaper);
    constexpr std::string_view Leave = "Esc leaves";
    Glass.Write(0, Glass.Cols - static_cast<std::uint32_t>(Leave.size()) - 2, Leave, TitleInk, BarPaper);
}

/* How far along, in parts of a thousand: by the clock in a timed test, by the
 * text otherwise; a timed test on a file ends with whichever comes first. */
std::uint64_t Progress(const TypingView &View)
{
    const Attempt &Run = View.Run;
    std::uint64_t  ByText = Run.Text().empty() ? 0 : Run.Cursor() * 1000ull / Run.Text().size();
    if (!Run.Timed()) return ByText;
    std::uint64_t ByClock = std::min<std::uint64_t>(Run.ElapsedMs(View.NowMs) * 1000 / Run.LimitMs(), 1000);
    return View.Job.Kind == TextKind::File ? std::max(ByText, ByClock) : ByClock;
}

void PaintBar(Screen &Glass, const TypingView &View, std::uint32_t Left, std::uint32_t Width)
{
    std::uint32_t Filled = static_cast<std::uint32_t>(Progress(View) * Width / 1000);
    for (std::uint32_t I = 0; I < Width; I++) {
        if (I < Filled) {
            PainterColor Fade = PainterColor::Mix(BarFilled, AmberInk, static_cast<std::uint8_t>(I * 255 / Width));
            Glass.Put(BarRow, Left + I, FullBlock, Fade, Paper);
        } else {
            Glass.Put(BarRow, Left + I, LightShade, BarEmpty, Paper);
        }
    }
}

void PaintChar(Screen &Glass, std::uint32_t Row, std::uint32_t Col, const Attempt &Run, std::size_t Index)
{
    char Wanted = Run.Text()[Index];
    if (Index == Run.Cursor()) {
        Glass.Put(Row, Col, Wanted, Paper, CaretPaper);
        return;
    }
    switch (Run.MarkAt(Index)) {
    case Mark::Right:   Glass.Put(Row, Col, Wanted, RightInk, Paper); break;
    case Mark::Wrong:   Glass.Put(Row, Col, Wanted, TitleInk, WrongPaper); break;
    case Mark::Untyped: Glass.Put(Row, Col, Wanted, RestInk, Paper); break;
    }
}

std::size_t LineOfCursor(const std::vector<Line> &Lines, std::size_t Cursor)
{
    for (std::size_t I = 0; I < Lines.size(); I++)
        if (Cursor < Lines[I].Start + Lines[I].Length) return I;
    return Lines.empty() ? 0 : Lines.size() - 1;
}

/* A line of text and a blank one between, so the eye keeps its place. The
 * passage is centred in its rows when it all fits, and scrolls with the cursor
 * on its second line when it does not. */
void PaintText(Screen &Glass, const TypingView &View, std::uint32_t Left)
{
    const std::vector<Line> &Lines = View.Lines;
    std::uint32_t Area    = Glass.Rows - TextRow - BelowText;
    std::size_t   Visible = (Area + 1) / 2;
    std::size_t   Current = LineOfCursor(Lines, View.Run.Cursor());

    std::size_t Top = Current > 0 ? Current - 1 : 0;
    if (Lines.size() > Visible) Top = std::min(Top, Lines.size() - Visible);
    std::size_t Shown = std::min(Visible, Lines.size() - Top);

    std::uint32_t Row = TextRow;
    if (Lines.size() <= Visible) Row += static_cast<std::uint32_t>((Area - (Shown * 2 - 1)) / 2);

    for (std::size_t I = 0; I < Shown; I++, Row += 2) {
        const Line &Text = Lines[Top + I];
        for (std::size_t At = 0; At < Text.Length; At++)
            PaintChar(Glass, Row, Left + static_cast<std::uint32_t>(At), View.Run, Text.Start + At);
    }
}

void PaintFoot(Screen &Glass, const TypingView &View)
{
    const Attempt &Run     = View.Run;
    std::uint64_t  Elapsed = Run.ElapsedMs(View.NowMs);
    unsigned       Accuracy = Run.Accuracy();

    std::vector<Figure> Figures;
    if (Run.Timed()) Figures.push_back({"left", SecondsText(Run.LimitMs() - Elapsed) + " s", BrightInk});
    else             Figures.push_back({"time", SecondsText(Elapsed) + " s", BrightInk});
    Figures.push_back({"speed", std::to_string(Run.Wpm(View.NowMs)) + " wpm", AmberInk});
    Figures.push_back({"accuracy", std::to_string(Accuracy) + "%", Accuracy >= 97 ? GoodInk : Accuracy >= 90 ? AmberInk : BadInk});
    Figures.push_back({"mistakes", std::to_string(Run.Mistakes()), Run.Mistakes() == 0 ? GoodInk : BadInk});

    Glass.Spread(Glass.Rows - 2, Figures);
}

}

std::uint32_t TextWidth(std::uint32_t Cols)
{
    return std::min(Cols - 2 * MarginLeast, WidthMost);
}

void PaintTyping(Screen &Glass, const TypingView &View)
{
    std::uint32_t Width = TextWidth(Glass.Cols);
    std::uint32_t Left  = (Glass.Cols - Width) / 2;

    Glass.Clear(Paper);
    PaintTitle(Glass, View.Job);
    PaintBar(Glass, View, Left, Width);
    PaintText(Glass, View, Left);

    std::string_view Under = View.Note;
    if (Under.empty() && !View.Run.Started()) Under = "The clock starts at your first key.";
    Glass.Centre(Glass.Rows - 3, Under, View.Note.empty() ? HintInk : AmberInk, Paper);
    PaintFoot(Glass, View);
    Glass.Centre(Glass.Rows - 1, "Backspace takes a character back", HintInk, Paper);
}
