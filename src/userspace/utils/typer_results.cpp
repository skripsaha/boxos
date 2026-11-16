/* typer_results.cpp — the screen after a run: speed and accuracy in numbers
 * five rows tall, what was typed, the keys that went wrong most, a bar chart of
 * speed over the run, and where the run stands against the record. A run that
 * beats the record gets a banner that changes colour with every beat and
 * sparks that fall on the empty glass around it. */

#include <algorithm>
#include <array>
#include <string>

#include "box/color.h"

#include "typer_screen.h"

namespace {

constexpr char32_t FullBlock = U'\u2588';   /* █ */
constexpr char32_t LowerHalf = U'\u2584';   /* ▄ */

constexpr std::uint32_t BigRows       = 5;
constexpr std::uint32_t BigScaleMost  = 2;
constexpr std::uint32_t BigMargin     = 4;
constexpr std::uint32_t ChartRowsLeast = 5;
constexpr std::uint32_t ChartRowsMost  = 10;
constexpr std::uint32_t ChartAxisWide  = 6;
constexpr std::uint32_t RowsUsedLeast  = 25;
constexpr std::size_t   MissedShown    = 5;

constexpr PainterColor ChartLow  = PainterColor::Rgb(0x20, 0x70, 0xB0);
constexpr PainterColor MissInk   = PainterColor::Rgb(0xFF, 0x90, 0x90);

constexpr std::array<std::array<std::string_view, BigRows>, 11> BigGlyphs = {{
    {"###", "#.#", "#.#", "#.#", "###"},
    {".#.", "##.", ".#.", ".#.", "###"},
    {"###", "..#", "###", "#..", "###"},
    {"###", "..#", "###", "..#", "###"},
    {"#.#", "#.#", "###", "..#", "..#"},
    {"###", "#..", "###", "..#", "###"},
    {"###", "#..", "###", "#.#", "###"},
    {"###", "..#", ".#.", ".#.", ".#."},
    {"###", "#.#", "###", "#.#", "###"},
    {"###", "#.#", "###", "..#", "###"},
    {"#.#", "..#", ".#.", "#..", "#.#"},
}};

const std::array<std::string_view, BigRows> &GlyphOf(char Ch)
{
    return BigGlyphs[Ch == '%' ? 10 : static_cast<std::size_t>(Ch - '0')];
}

/* A pixel of a big digit is two columns wide and one row tall, which is square
 * on an 8x16 cell; at scale 2 it is four by two. The gap between digits is one
 * pixel, between the two figures three. */
std::uint32_t PixelWide(std::uint32_t Scale) { return 2 * Scale; }
std::uint32_t BigPitch(std::uint32_t Scale) { return 4 * PixelWide(Scale); }
std::uint32_t BlocksGap(std::uint32_t Scale) { return 6 * PixelWide(Scale); }

std::uint32_t BigWidth(std::string_view Number, std::uint32_t Scale)
{
    return Number.empty() ? 0 : static_cast<std::uint32_t>(Number.size()) * BigPitch(Scale) - PixelWide(Scale);
}

void PaintBig(Screen &Display, std::uint32_t Row, std::uint32_t Col, std::string_view Number, PainterColor Ink, std::uint32_t Scale)
{
    for (std::size_t At = 0; At < Number.size(); At++) {
        const auto &Glyph = GlyphOf(Number[At]);
        for (std::uint32_t Y = 0; Y < BigRows * Scale; Y++) {
            PainterColor Shade = PainterColor::Mix(Ink, painter::core::Colors::White, static_cast<std::uint8_t>(60 - Y / Scale * 15));
            for (std::uint32_t X = 0; X < 3; X++)
                if (Glyph[Y / Scale][X] == '#')
                    Display.Fill(Row + Y, Col + static_cast<std::uint32_t>(At) * BigPitch(Scale) + X * PixelWide(Scale),
                               PixelWide(Scale), FullBlock, Shade, Paper);
        }
    }
}

std::string SpeedText(const Report &Result) { return std::to_string(Result.Run.Wpm(Result.ElapsedMs)); }
std::string AccuracyText(const Report &Result) { return std::to_string(Result.Run.Accuracy()) + "%"; }

/* Big when the screen has rows to spare and the figures fit across it. */
std::uint32_t BigScaleFor(const Screen &Display, const Report &Result)
{
    std::uint32_t Spare = Display.Rows > RowsUsedLeast ? Display.Rows - RowsUsedLeast : 0;
    for (std::uint32_t Scale = BigScaleMost; Scale > 1; Scale--) {
        std::uint32_t Across = BigWidth(SpeedText(Result), Scale) + BlocksGap(Scale) + BigWidth(AccuracyText(Result), Scale);
        if (Spare >= BigRows * (Scale - 1) + 1 && Across + 2 * BigMargin <= Display.Cols) return Scale;
    }
    return 1;
}

PainterColor AccuracyInk(unsigned Accuracy)
{
    return Accuracy >= 97 ? GoodInk : Accuracy >= 90 ? AmberInk : BadInk;
}

/* Two big numbers side by side, each with its words centred under it. */
void PaintNumbers(Screen &Display, std::uint32_t Row, const Report &Result, std::uint32_t Scale)
{
    std::string Speed    = SpeedText(Result);
    std::string Accuracy = AccuracyText(Result);

    std::uint32_t Total = BigWidth(Speed, Scale) + BlocksGap(Scale) + BigWidth(Accuracy, Scale);
    std::uint32_t Left  = (Display.Cols - Total) / 2;
    std::uint32_t Right = Left + BigWidth(Speed, Scale) + BlocksGap(Scale);

    PaintBig(Display, Row, Left, Speed, AmberInk, Scale);
    PaintBig(Display, Row, Right, Accuracy, AccuracyInk(Result.Run.Accuracy()), Scale);

    auto Under = [&](std::uint32_t Col, std::uint32_t Width, std::string_view Words) {
        std::int32_t Start = static_cast<std::int32_t>(Col + Width / 2) - static_cast<std::int32_t>(Words.size() / 2);
        Display.Write(Row + BigRows * Scale, static_cast<std::uint32_t>(std::max(Start, 0)), Words, HintInk, Paper);
    };
    Under(Left, BigWidth(Speed, Scale), "words per minute");
    Under(Right, BigWidth(Accuracy, Scale), "accuracy");
}

std::string KeyName(char Key)
{
    return Key == ' ' ? "space" : std::string(1, Key);
}

void PaintMissed(Screen &Display, std::uint32_t Row, const Attempt &Run)
{
    std::vector<Missed> Worst = Run.MostMissed(MissedShown);
    if (Worst.empty()) {
        Display.Centre(Row, "no key missed - a clean run", GoodInk, Paper);
        return;
    }

    std::vector<Figure> Keys;
    for (const Missed &One : Worst) Keys.push_back({KeyName(One.Key), "x" + std::to_string(One.Count), MissInk});
    Display.Centre(Row - 1, "keys missed most", HintInk, Paper);
    Display.Spread(Row, Keys);
}

/* A bar is built from the bottom in half rows, so a chart five rows tall tells
 * ten speeds apart. */
void PaintBar(Screen &Display, std::uint32_t Bottom, std::uint32_t Left, std::uint32_t Wide, std::uint32_t Rows,
              unsigned Speed, unsigned Top)
{
    unsigned Halves = (Speed * Rows * 2 + Top / 2) / Top;
    if (Speed != 0 && Halves == 0) Halves = 1;
    PainterColor Ink = PainterColor::Mix(ChartLow, AmberInk, static_cast<std::uint8_t>(Speed * 255 / Top));
    for (std::uint32_t Up = 0; Up < Rows; Up++) {
        char32_t Ch = Halves >= 2 * (Up + 1) ? FullBlock : Halves == 2 * Up + 1 ? LowerHalf : 0;
        if (Ch != 0) Display.Fill(Bottom - Up, Left, Wide, Ch, Ink, Paper);
    }
}

/* Returns the row below the chart. */
std::uint32_t PaintChart(Screen &Display, std::uint32_t Row, std::uint32_t Rows, const Report &Result)
{
    std::uint32_t Room   = std::min<std::uint32_t>(Display.Cols - 16, 60);
    unsigned      Slice  = SliceSecondsFor(Result.ElapsedMs, Room / 3);
    std::vector<unsigned> Speeds = Result.Run.SpeedBars(Slice, Result.ElapsedMs);
    if (Speeds.empty()) return Row;

    unsigned Top = std::max(*std::max_element(Speeds.begin(), Speeds.end()), 1u);
    std::uint32_t Pitch = std::min<std::uint32_t>(Room / static_cast<std::uint32_t>(Speeds.size()), 6);
    std::uint32_t Width = static_cast<std::uint32_t>(Speeds.size()) * Pitch;
    std::uint32_t Left  = (Display.Cols - Width - ChartAxisWide) / 2 + ChartAxisWide;

    Display.Centre(Row, "speed over the run, one bar for every " + std::to_string(Slice) + " s", HintInk, Paper);
    std::uint32_t Bottom = Row + Rows;
    for (std::size_t I = 0; I < Speeds.size(); I++)
        PaintBar(Display, Bottom, Left + static_cast<std::uint32_t>(I) * Pitch, Pitch - 1, Rows, Speeds[I], Top);

    Display.Write(Row + 1, Left - ChartAxisWide, std::to_string(Top), HintInk, Paper);
    Display.Write(Bottom, Left - ChartAxisWide, "0", HintInk, Paper);
    std::string End = SecondsText(Result.ElapsedMs) + " s";
    if (Width > End.size() + 4) Display.Write(Bottom + 1, Left, "0 s", HintInk, Paper);
    Display.Write(Bottom + 1, Left + Width - static_cast<std::uint32_t>(End.size()), End, HintInk, Paper);
    return Bottom + 2;
}

void PaintStanding(Screen &Display, std::uint32_t Row, const Report &Result, unsigned Beat)
{
    switch (Result.Place) {
    case Standing::Beaten: {
        constexpr std::string_view Banner = "*  N E W   P E R S O N A L   B E S T  *";
        std::uint32_t Col = Display.Centre(Row, Banner, Paper, Paper);
        for (std::size_t I = 0; I < Banner.size(); I++) {
            PainterColor Hue = PainterColor::FromRaw(
                color_wheel(static_cast<std::uint16_t>((Beat * 90 + I * 40) % 1536), 220, 255));
            Display.Put(Row, Col + static_cast<std::uint32_t>(I), Banner[I], Hue, Paper);
        }
        Display.Centre(Row + 1, "the old record was " + std::to_string(Result.Best) + " wpm", HintInk, Paper);
        break;
    }
    case Standing::First:
        Display.Centre(Row, "The first run recorded for this kind of text.", GoodInk, Paper);
        break;
    case Standing::Short:
        Display.Centre(Row, "Your record for this kind of text is " + std::to_string(Result.Best) + " wpm.", HintInk, Paper);
        break;
    case Standing::NoRecord:
        break;
    }
}

/* Sparks on the empty glass only: the numbers are never written over. */
void Sparkle(Screen &Display, unsigned Beat)
{
    constexpr std::array<char32_t, 4> Shapes = {U'*', U'.', U'+', U'\u263C'};
    Dice Chance(Beat * 7919ull + 1);
    std::uint32_t Count = Display.Rows * Display.Cols / 50;
    for (std::uint32_t I = 0; I < Count; I++) {
        std::uint32_t Row = static_cast<std::uint32_t>(Chance.Below(Display.Rows));
        std::uint32_t Col = static_cast<std::uint32_t>(Chance.Below(Display.Cols));
        const painter::tui::Cell Under = Display.At(Row, Col);
        if (Under.Glyph != ' ' || Under.Background != Paper) continue;
        PainterColor Hue = PainterColor::FromRaw(
            color_wheel(static_cast<std::uint16_t>(Chance.Below(1536)), 200, 255));
        Display.Put(Row, Col, Shapes[Chance.Below(Shapes.size())], Hue, Paper);
    }
}

void PaintKeys(Screen &Display, std::uint32_t Row)
{
    std::uint32_t Col = (Display.Cols - 44) / 2;
    Col = Display.Write(Row, Col, "Enter", BrightInk, Paper) + 1;
    Col = Display.Write(Row, Col, "another passage", HintInk, Paper) + 6;
    Col = Display.Write(Row, Col, "Esc", BrightInk, Paper) + 1;
    Col = Display.Write(Row, Col, "or", HintInk, Paper) + 1;
    Col = Display.Write(Row, Col, "q", BrightInk, Paper) + 1;
    Display.Write(Row, Col, "leave", HintInk, Paper);
}

}

void PaintResults(Screen &Display, const Report &Result, unsigned Beat)
{
    const Attempt &Run = Result.Run;
    std::uint32_t  Scale = BigScaleFor(Display, Result);
    std::uint32_t  Spare = Display.Rows > RowsUsedLeast + BigRows * (Scale - 1) ? Display.Rows - RowsUsedLeast - BigRows * (Scale - 1) : 0;
    std::uint32_t  ChartRows = std::min(ChartRowsLeast + Spare / 2, ChartRowsMost);
    std::uint32_t  Pad = std::min<std::uint32_t>((Spare - (ChartRows - ChartRowsLeast)) / 2, 3);

    Display.Clear(Paper);
    Display.Fill(0, 0, Display.Cols, ' ', TitleInk, BarPaper);
    std::uint32_t Col = Display.Write(0, 2, "typer", AmberInk, BarPaper);
    Display.Write(0, Col + 3, "results  -  " + KindLabel(Result.Job), TitleInk, BarPaper);

    std::uint32_t Row = 1 + Pad;
    PaintStanding(Display, Row, Result, Beat);
    Row += 2;
    PaintNumbers(Display, Row, Result, Scale);
    Row += BigRows * Scale + 2;

    Display.Spread(Row, {{"typed", std::to_string(Run.Keys()) + " characters", BrightInk},
                       {"time", SecondsText(Result.ElapsedMs) + " s", BrightInk},
                       {"mistakes", std::to_string(Run.Mistakes()), Run.Mistakes() == 0 ? GoodInk : BadInk},
                       {"corrections", std::to_string(Run.Corrections()), BrightInk}});
    Row += 3;
    PaintMissed(Display, Row, Run);
    Row += 2;
    PaintChart(Display, Row, ChartRows, Result);

    if (!Result.Trouble.empty()) Display.Centre(Display.Rows - 2, Result.Trouble, AmberInk, Paper);
    PaintKeys(Display, Display.Rows - 1);
    if (Result.Place == Standing::Beaten) Sparkle(Display, Beat);
}
