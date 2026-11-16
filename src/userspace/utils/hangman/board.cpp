/* board.cpp — the words beside the scene (board.h), set out in sections down
 * one column: a heading, the word, a card for the lives and the letters said
 * wrong, a card for the alphabet and the hint, a card for the standing, and a
 * line for what was just said. Each card has its label at the left, and the
 * labels and their values line up. */

#include "board.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

#include "numbers.h"
#include "pen.h"
#include "slots.h"

namespace hangman {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;
namespace G = painter::tui::Glyphs;

constexpr char32_t HeartGlyph = U'\u2665';    // ♥ a life left
constexpr char32_t SquareGlyph = U'\u25A0';   // ■
constexpr std::uint64_t RefusedMs = 380;
constexpr std::uint64_t NoteMs = 5000;
constexpr std::int32_t  MostColumn = 64;   // the widest the sections are, in cells
constexpr std::int32_t  LabelColumn = 10;  // where a card's values begin, from its margin
constexpr std::int32_t  CardMargin = 2;

// The sections' heights, from the top: the heading, the word, the lives, the alphabet, the standing, the note.
constexpr std::int32_t HeadingRows = 1;
constexpr std::int32_t LivesRows = 2;
constexpr std::int32_t LettersRows = 8;
constexpr std::int32_t StandingRows = 2;
constexpr std::int32_t NoteRows = 1;
constexpr std::int32_t GapCount = 5;          // between the six sections
constexpr std::int32_t MostGap = 3;
constexpr std::int32_t CardPads = 6;          // the rows a card may be padded by, top and bottom, for the three
constexpr std::int32_t BoardRows = HeadingRows + SlotRows + LivesRows + LettersRows + StandingRows + NoteRows;

std::string Capitals(std::string_view Words)
{
    std::string Loud(Words);
    for (char &Letter : Loud) Letter = Round::Upper(Letter);
    return Loud;
}

bool IsOver(const Facts &Now)
{
    return Now.Playing.State() != Phase::Playing;
}

// The label of a card's row, at its left.
void Label(const Pen &Card, const Facts &Now, std::int32_t Row, std::string_view Words)
{
    Card.From(Card.Area.X + CardMargin, Row, {{std::string(Words), Now.Shown.Quiet}});
}

Color LevelInk(const Facts &Now)
{
    switch (Now.Playing.Hardness()) {
    case Level::Easy:   return Now.Shown.Good;
    case Level::Medium: return Now.Shown.Title;
    case Level::Hard:   return Now.Shown.Bad;
    }
    return Now.Shown.Text;
}

// The category as a chip at the left, and how hard the word is and how long, at the right.
void PaintHeading(const Pen &Page, const Facts &Now, std::int32_t Row)
{
    const Look  &Shown = Now.Shown;
    const Round &Word = Now.Playing;
    const std::string Chip = " " + Capitals(Word.Label()) + " ";
    tui::PutText(Page.Out, {Page.Area.X, Row}, Chip, Page.Area.Width, Shown.Bars, Shown.Title);
    Page.Right(Row, {{std::string(LevelName(Word.Hardness())), LevelInk(Now)}, {std::format("   {} letters", Word.Word().size()), Shown.Quiet}});
}

void PaintLives(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    const Look  &Shown = Now.Shown;
    const Round &Word = Now.Playing;
    const std::int32_t Left = Card.Area.X + CardMargin + LabelColumn;
    Label(Card, Now, Row, "LIVES");
    for (int Pip = 0; Pip < MostMistakes; Pip++) {
        const bool Lost = Pip >= MostMistakes - Word.Mistakes();
        Card.Out.Put({Left + Pip * 2, Row}, Lost ? G::Dot : HeartGlyph, {Lost ? Mixed(Shown.Quiet, Card.Paper, 0.4f) : Shown.Bad, Card.Paper});
    }
    Card.From(Left + MostMistakes * 2 + 1, Row, {{std::format("{} of {}", MostMistakes - Word.Mistakes(), MostMistakes), Shown.Quiet}});
    Label(Card, Now, Row + 1, "MISSED");
    if (Word.Misses().empty()) {
        Card.From(Left, Row + 1, {{"none yet", Shown.Quiet}});
        return;
    }
    std::string Missed;
    for (const char Letter : Word.Misses()) Missed += std::string(1, Letter) + ' ';
    Card.From(Left, Row + 1, {{Missed, Shown.Bad}});
}

Color TileOf(const Pen &Card, const Facts &Now, char Letter, Color &Face)
{
    const Look  &Shown = Now.Shown;
    const Round &Word = Now.Playing;
    Color Tile = Mixed(Card.Paper, Shown.Text, 0.14f);
    Face = Shown.Text;
    if (Word.Found(Letter)) {
        Tile = Mixed(Card.Paper, Shown.Good, 0.4f);
        Face = Lighter(Shown.Good, 0.5f);
    } else if (Word.Failed(Letter)) {
        Tile = Mixed(Card.Paper, Shown.Bad, 0.3f);
        Face = Mixed(Shown.Bad, Card.Paper, 0.35f);
    }
    if (Now.Refused == Letter && Since(Now, Now.RefusedAt) < RefusedMs)
        Tile = Mixed(Tile, Shown.Title, 0.6f * (1.0f - static_cast<float>(Since(Now, Now.RefusedAt)) / static_cast<float>(RefusedMs)));
    return Tile;
}

// A key of the alphabet: a tile a cell and a half wider than its letter, its ends half-cells.
void PaintTile(const Pen &Card, const Facts &Now, char Letter, std::int32_t X, std::int32_t Row)
{
    Color       Face;
    const Color Tile = TileOf(Card, Now, Letter, Face);
    Card.Out.Put({X, Row}, G::RightHalf, {Tile, Card.Paper});
    Card.Out.Put({X + 1, Row}, Letter, {Face, Tile});
    Card.Out.Put({X + 2, Row}, G::LeftHalf, {Tile, Card.Paper});
}

void PaintAlphabet(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    static constexpr std::string_view Keyboard[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static constexpr std::string_view Plain[] = {"ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ"};
    const std::string_view *Rows = Now.Shown.Qwerty ? Keyboard : Plain;
    for (int Line = 0; Line < 3; Line++) {
        const std::string_view Letters = Rows[Line];
        const std::int32_t     Start = Card.Area.X + (Card.Area.Width - static_cast<std::int32_t>(Letters.size()) * 3) / 2;
        for (std::size_t At = 0; At < Letters.size(); At++) PaintTile(Card, Now, Letters[At], Start + static_cast<std::int32_t>(At) * 3, Row + Line * 2);
    }
}

void PaintVerdict(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    const Look  &Shown = Now.Shown;
    const Round &Word = Now.Playing;
    const bool   Won = Word.State() == Phase::Won;
    Card.Centred(Row, {{Won ? "H E  I S  F R E E !" : "T H E  M A N  I S  L O S T", Won ? Shown.Good : Shown.Bad}});
    Card.Centred(Row + 1, {{"the word was ", Shown.Quiet}, {Capitals(Word.Word()), Shown.Title}});
    if (Won) Card.Centred(Row + 2, {{std::format("+{} points", Now.Earned), Shown.Good}});
    else Card.Centred(Row + 2, {{"no points this time", Shown.Quiet}});
    if (!Now.Counted) Card.Centred(Row + 3, {{"a friend's word is not counted", Shown.Quiet}});
    else Card.Centred(Row + 3, {{std::format("streak {}", Now.Total.Streak), Shown.Text}});
    Card.Centred(Row + 4, {{"enter", Shown.Title}, {" for the next word", Shown.Text}});
}

// What the colours of the tiles mean, at the right of the card's top row. A square is a glyph of the
// font's own, not a character the text writer knows.
void PaintLegend(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    const Look &Shown = Now.Shown;
    const std::string_view Good = " in the word   ", Bad = " not in it";
    const std::int32_t Start = Card.Area.X + Card.Area.Width - 1 - (2 + Pen::Cells(Good)) - (1 + Pen::Cells(Bad));
    Card.Out.Put({Start, Row}, SquareGlyph, {Shown.Good, Card.Paper});
    Card.From(Start + 1, Row, {{std::string(Good), Shown.Quiet}});
    Card.Out.Put({Start + 1 + Pen::Cells(Good), Row}, SquareGlyph, {Shown.Bad, Card.Paper});
    Card.From(Start + 2 + Pen::Cells(Good), Row, {{std::string(Bad), Shown.Quiet}});
}

void PaintHint(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    const Look &Shown = Now.Shown;
    if (Shown.HintsAllowed == 0) return;
    Label(Card, Now, Row, "HINT");
    const std::int32_t Left = Card.Area.X + CardMargin + LabelColumn;
    if (Now.HintsLeft == 0) {
        Card.From(Left, Row, {{"none left for this word", Shown.Quiet}});
        return;
    }
    const std::string Cost = Shown.Price == HintCost::Points ? std::format("-{} pts", HintPoints) : "costs a mistake";
    Card.From(Left, Row, {{std::string(Now.HintKey), Shown.Title}, {std::format("  {} left, {}", Now.HintsLeft, Cost), Shown.Quiet}});
}

// The alphabet and the hint, or once the round is over how it ended.
void PaintLetters(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    if (IsOver(Now)) {
        Label(Card, Now, Row, "RESULT");
        PaintVerdict(Card, Now, Row + 1);
        return;
    }
    Label(Card, Now, Row, "LETTERS");
    PaintLegend(Card, Now, Row);
    PaintAlphabet(Card, Now, Row + 1);
    PaintHint(Card, Now, Row + 7);
}

// Four figures, each under its name, spread across the card.
void PaintStanding(const Pen &Card, const Facts &Now, std::int32_t Row)
{
    const Look  &Shown = Now.Shown;
    const Tally &Total = Now.Total;
    const std::string_view Names[] = {"WON", "STREAK", "BEST", "SCORE"};
    const std::string      Values[] = {std::format("{} of {}", Total.Wins, Total.Games), std::format("{}", Total.Streak),
                                       std::format("{}", Total.Best), std::format("{}", Total.Score)};
    const std::int32_t     Slice = Card.Area.Width / 4;
    for (std::int32_t At = 0; At < 4; At++) {
        const std::int32_t Middle = Card.Area.X + Slice * At + Slice / 2;
        Card.Over(Middle, Row, {{std::string(Names[At]), Shown.Quiet}});
        Card.Over(Middle, Row + 1, {{Values[static_cast<std::size_t>(At)], At == 3 ? Shown.Title : Shown.Text}});
    }
}

void PaintNote(const Pen &Page, const Facts &Now, std::int32_t Row)
{
    if (Now.Note.empty() || Since(Now, Now.NoteAt) > NoteMs) return;
    const Look &Shown = Now.Shown;
    Color Shade = Shown.Text;
    switch (Now.NoteWas) {
    case NoteKind::Plain: break;
    case NoteKind::Good:  Shade = Shown.Good; break;
    case NoteKind::Bad:   Shade = Shown.Bad; break;
    case NoteKind::Warn:  Shade = Shown.Title; break;
    }
    const float Fade = Smooth(3500.0f, static_cast<float>(NoteMs), static_cast<float>(Since(Now, Now.NoteAt)));
    Page.Centred(Row, {{std::string(Now.Note), Mixed(Shade, Page.Paper, Fade)}});
}

}  // namespace

Color PaperOf(const Look &Shown)
{
    return Mixed(Shown.Bars, Shown.Sky, 0.12f);
}

void PaintBoard(tui::Frame &Out, core::Rect Area, const Facts &Now)
{
    const Color        Paper = PaperOf(Now.Shown);
    const Color        Card = Mixed(Paper, Now.Shown.Text, 0.07f);
    const std::int32_t Wide = std::min(Area.Width, MostColumn);
    const std::int32_t Left = Area.X + (Area.Width - Wide) / 2;
    const std::int32_t Pad = Area.Height >= BoardRows + CardPads + GapCount ? 1 : 0;
    std::int32_t       Gaps[GapCount] = {0, 0, 0, 0, 0};
    std::int32_t       Spare = Area.Height - BoardRows - CardPads * Pad;
    for (std::int32_t At = 0; Spare > 0 && At < GapCount * MostGap; At++, Spare--) Gaps[At % GapCount]++;
    std::int32_t Row = Area.Y + std::max(0, Spare / 2);

    PaintHeading(Pen{Out, {Left, Row, Wide, HeadingRows}, Paper}, Now, Row);
    Row += HeadingRows + Gaps[0];
    PaintSlots(Pen{Out, {Area.X, Row, Area.Width, SlotRows}, Paper}, Now, Row);
    Row += SlotRows + Gaps[1];
    PaintLives(Pen::Laid(Out, {Left, Row, Wide, LivesRows + 2 * Pad}, Card), Now, Row + Pad);
    Row += LivesRows + 2 * Pad + Gaps[2];
    PaintLetters(Pen::Laid(Out, {Left, Row, Wide, LettersRows + 2 * Pad}, Card), Now, Row + Pad);
    Row += LettersRows + 2 * Pad + Gaps[3];
    PaintStanding(Pen::Laid(Out, {Left, Row, Wide, StandingRows + 2 * Pad}, Card), Now, Row + Pad);
    Row += StandingRows + 2 * Pad + Gaps[4];
    PaintNote(Pen{Out, {Left, Row, Wide, NoteRows}, Paper}, Now, Row);
}

void PaintTopBar(tui::Frame &Out, const Look &Shown)
{
    const std::int32_t Width = Out.Size().Width;
    Out.Fill({0, 0, Width, 1}, ' ', {Shown.Text, Shown.Bars});
    tui::PutText(Out, {1, 0}, "H A N G M A N", Width - 2, Shown.Title, Shown.Bars);
}

void PaintHintBar(tui::Frame &Out, const Look &Shown, std::string_view Text)
{
    const core::Size Size = Out.Size();
    Out.Fill({0, Size.Height - 1, Size.Width, 1}, ' ', {Shown.Quiet, Shown.Bars});
    tui::PutText(Out, {1, Size.Height - 1}, Text, Size.Width - 2, Shown.Quiet, Shown.Bars);
}

}  // namespace hangman
