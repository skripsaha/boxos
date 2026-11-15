/* tetris — the falling pieces, on the glass.
 *
 *   left, right     move
 *   up, x           turn clockwise        z    turn the other way
 *   down            down faster           space  all the way down
 *   c               hold the piece for later, once a piece
 *   p               pause                 r    a new game
 *   q, ESC          leave; the best score is kept
 *
 * The game is tetris_play.cpp; this is the well on the glass, the pieces to
 * come, the one held, the score, and the best of them kept on the volume in
 * tetris.record (keep.h). */

#include <algorithm>
#include <charconv>
#include <format>
#include <new>
#include <string>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "keep.h"
#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/stage.h"
#include "tetris.h"

BOX_FORM("");

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

constexpr std::uint64_t FrameUs  = 16667;
constexpr float         MostStep = 0.05f;
const Keep              Record{"tetris.record", "tetris,record"};

const Color Page     = Color::Rgb(0x0C, 0x0C, 0x0C);
const Color WellInk  = Color::Rgb(0x10, 0x10, 0x10);
const Color GridInk  = Color::Rgb(0x34, 0x34, 0x34);
const Color FrameInk = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color LabelInk = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color ValueInk = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color KeyInk   = Colors::Amber;
const Color TitleInk = Color::Rgb(0x40, 0xE0, 0xFF);
const Color FlashInk = Color::Rgb(0xFF, 0xFF, 0xFF);

/* The guideline's colours: I cyan, O yellow, T purple, S green, Z red, J blue,
 * L orange. All light but orange, so on a text console L is the one brown and
 * O stays yellow. */
const Color PieceInks[] = {Color::Rgb(0x40, 0xE8, 0xF8), Color::Rgb(0xF0, 0xE0, 0x40), Color::Rgb(0xA8, 0x30, 0xF0),
                           Color::Rgb(0x30, 0xE0, 0x40), Color::Rgb(0xF0, 0x20, 0x20), Color::Rgb(0x30, 0x60, 0xFF),
                           Color::Rgb(0xF0, 0x90, 0x00)};

/* The double frame's box-drawing characters. */
constexpr char32_t FrameSide = U'\u2551', FrameTop = U'\u2550';   /* ║ ═ */
constexpr char32_t CornerTL = U'\u2554', CornerTR = U'\u2557';    /* ╔ ╗ */
constexpr char32_t CornerBL = U'\u255A', CornerBR = U'\u255D';    /* ╚ ╝ */

class Table {
public:
    std::optional<Stage> Screen;
    Well                Game;
    unsigned Best   = 0;
    bool     Paused = false;
    float    Clock  = 0;

    void Measure()
    {
        const painter::core::Size Dimensions = Screen->Size();
        Scale = std::max(1, std::min((Dimensions.Height - 2) / WellHigh, (Dimensions.Width - 44) / (2 * WellWide)));
        Wide  = 2 * Scale * WellWide + 2;
        High  = Scale * WellHigh + 2;
        Left  = (Dimensions.Width - Wide) / 2;
        Top   = std::max(0, (Dimensions.Height - High) / 2);
    }

    void Draw();

private:
    void Block(int Row, int Col, Color Ink, int Size);
    void Mini(int Row, int Col, Piece Kind, bool Dim);
    void Frame();
    void Panels();
    void Overlay(std::string_view Line, std::string_view Hint);

    int Scale = 1, Wide = 0, High = 0, Left = 0, Top = 0;
};

/* A block of `Size` rows, twice as many columns; a big one with its top lit
 * and its foot shaded, so the pile reads as blocks and not as paint. */
void Table::Block(int Row, int Col, Color Ink, int Size)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    if (Size == 1) {
        FrameBuffer.Fill({Col, Row, 2, 1}, ' ', TextStyle{Ink, Ink});
        return;
    }
    Color Lit = Color::Mix(Ink, Colors::White, 90), Shade = Color::Mix(Ink, Colors::Black, 70);
    for (int R = 0; R < Size; R++) {
        for (int C = 0; C < 2 * Size; C++) {
            if (R == 0) FrameBuffer.Put({Col + C, Row}, painter::tui::Glyphs::UpperHalf, TextStyle{Lit, Ink});
            else if (R == Size - 1)
                FrameBuffer.Put({Col + C, Row + R}, painter::tui::Glyphs::LowerHalf, TextStyle{Shade, Ink});
            else FrameBuffer.Put({Col + C, Row + R}, ' ', TextStyle{Ink, Ink});
        }
    }
}

/* A piece in its box, a block a row high: the held one and the ones to come. */
void Table::Mini(int Row, int Col, Piece Kind, bool Dim)
{
    if (Kind == Piece::None) return;
    Color Ink = PieceInks[static_cast<int>(Kind)];
    if (Dim) Ink = Color::Mix(Ink, Page, 150);
    for (const auto &Cell : PieceCells(Kind, 0)) Block(Row + Cell[0], Col + 2 * Cell[1], Ink, 1);
}

void Table::Frame()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const TextStyle FrameStyle{FrameInk, Page};
    FrameBuffer.Put({Left, Top}, CornerTL, FrameStyle);
    FrameBuffer.Put({Left + Wide - 1, Top}, CornerTR, FrameStyle);
    FrameBuffer.Put({Left, Top + High - 1}, CornerBL, FrameStyle);
    FrameBuffer.Put({Left + Wide - 1, Top + High - 1}, CornerBR, FrameStyle);
    FrameBuffer.Fill({Left + 1, Top, Wide - 2, 1}, FrameTop, FrameStyle);
    FrameBuffer.Fill({Left + 1, Top + High - 1, Wide - 2, 1}, FrameTop, FrameStyle);
    for (int Row = Top + 1; Row < Top + High - 1; Row++) {
        FrameBuffer.Put({Left, Row}, FrameSide, FrameStyle);
        FrameBuffer.Put({Left + Wide - 1, Row}, FrameSide, FrameStyle);
    }
}

void Table::Panels()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    const TextStyle TitleStyle{TitleInk, Page};
    const TextStyle LabelStyle{LabelInk, Page};
    const TextStyle ValueStyle{ValueInk, Page};
    const TextStyle KeyStyle{KeyInk, Page};
    int Side = Left - 14, Row = Top + 1;
    FrameBuffer.Write({Side, Row}, "TETRIS", TitleStyle);
    FrameBuffer.Write({Side, Row + 2}, "hold", LabelStyle);
    Mini(Row + 3, Side, Game.Held(), !Game.CanHold());
    struct Figure { const char *Label; unsigned Value; };
    const Figure Figures[] = {{"score", Game.Score}, {"lines", Game.Lines}, {"level", Game.Level}, {"best", Best}};
    Row += 7;
    for (const Figure &One : Figures) {
        FrameBuffer.Write({Side, Row}, One.Label, LabelStyle);
        FrameBuffer.Write({Side, Row + 1}, std::to_string(One.Value), ValueStyle);
        Row += 3;
    }

    int Right = Left + Wide + 3;
    Row = Top + 1;
    FrameBuffer.Write({Right, Row}, "next", LabelStyle);
    for (int At = 0; At < NextShown; At++) Mini(Row + 2 + At * 3, Right, Game.Upcoming(At), At > 0);

    Row = Top + 2 + NextShown * 3 + 1;
    for (auto [Keys, What] : {std::pair{"\u2190\u2192", "move"}, {"\u2191 x z", "turn"}, {"\u2193", "down"},
                              {"space", "drop"}, {"c", "hold"}, {"p", "pause"}, {"q", "leave"}}) {
        if (Row >= Dimensions.Height) break;
        int Col = Right;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Keys, KeyStyle));
        FrameBuffer.Write({Col + 1, Row++}, What, LabelStyle);
    }
}

void Table::Overlay(std::string_view Line, std::string_view Hint)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    int Row = Top + High / 2 - 2, Width = Wide - 2;
    FrameBuffer.Fill({Left + 1, Row, Width, 5}, ' ', TextStyle{ValueInk, Page});
    FrameBuffer.Write({Left + 1 + (Width - static_cast<int>(Line.size())) / 2, Row + 1}, Line,
                      TextStyle{TitleInk, Page});
    if (static_cast<int>(Hint.size()) <= Width)
        FrameBuffer.Write({Left + 1 + (Width - static_cast<int>(Hint.size())) / 2, Row + 3}, Hint,
                          TextStyle{LabelInk, Page});
}

void Table::Draw()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    FrameBuffer.Clear(TextStyle{Page, Page});
    Frame();
    Panels();

    const std::vector<int> &Clearing = Game.Clearing();
    bool Flash = static_cast<int>(Clock * 20.0f) % 2 == 0;
    for (int R = 0; R < WellHigh; R++) {
        int  Row      = R + WellHidden;
        bool Flashing = std::find(Clearing.begin(), Clearing.end(), Row) != Clearing.end();
        for (int C = 0; C < WellWide; C++) {
            int   Y = Top + 1 + R * Scale, X = Left + 1 + C * 2 * Scale;
            Piece Cell = Game.Cell(Row, C);
            if (Cell != Piece::None) {
                Block(Y, X, Flashing && Flash ? FlashInk : PieceInks[static_cast<int>(Cell)], Scale);
                continue;
            }
            FrameBuffer.Fill({X, Y, 2 * Scale, Scale}, ' ', TextStyle{WellInk, WellInk});
            FrameBuffer.Put({X + Scale, Y + Scale / 2}, painter::tui::Glyphs::Dot, TextStyle{GridInk, WellInk});
        }
    }

    const Falling &Now = Game.Now();
    if (Now.Kind != Piece::None) {
        Color Ink   = PieceInks[static_cast<int>(Now.Kind)];
        int   Ghost = Game.GhostRow();
        for (const auto &Cell : PieceCells(Now.Kind, Now.Turn)) {
            int R = Ghost + Cell[0] - WellHidden, C = Now.Col + Cell[1];
            if (R < 0) continue;
            int Y = Top + 1 + R * Scale, X = Left + 1 + C * 2 * Scale;
            FrameBuffer.Fill({X, Y, 2 * Scale, Scale}, painter::tui::Glyphs::LightShade,
                             TextStyle{Color::Mix(Ink, WellInk, 110), WellInk});
        }
        for (const auto &Cell : PieceCells(Now.Kind, Now.Turn)) {
            int R = Now.Row + Cell[0] - WellHidden, C = Now.Col + Cell[1];
            if (R >= 0) Block(Top + 1 + R * Scale, Left + 1 + C * 2 * Scale, Ink, Scale);
        }
    }

    if (Game.Over) Overlay("game over", "r again, q leave");
    else if (Paused) Overlay("paused", "p to go on");
}

unsigned ReadBest(std::string &Trouble)
{
    std::string Text;
    if (std::optional<std::string> Refusal = KeepRead(Record, Text)) {
        Trouble = *Refusal;
        return 0;
    }
    unsigned    Best = 0;
    std::size_t At   = Text.find("best ");
    if (At != std::string::npos) std::from_chars(Text.data() + At + 5, Text.data() + Text.size(), Best);
    return Best;
}

/* False to leave. */
bool OnKey(Table &Desk, const KeyEvent &Key)
{
    Well &Game = Desk.Game;
    if (Key.IsExtended()) {
        if (Desk.Paused) return true;
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  Game.Shift(-1); break;
        case painter::tui::KeyCodes::Right: Game.Shift(1); break;
        case painter::tui::KeyCodes::Down:  Game.SoftDrop(); break;
        case painter::tui::KeyCodes::Up:    Game.Turn(1); break;
        default: break;
        }
        return true;
    }
    char32_t Ch = Key.Character;
    if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    if (Ch == 'q' || Ch == painter::tui::KeyCodes::Escape) return false;
    if (Ch == 'r') {
        Game.Start(cpu_rdtsc());
        Desk.Paused = false;
    }
    if (Ch == 'p' && !Game.Over) Desk.Paused = !Desk.Paused;
    if (Desk.Paused) return true;
    if (Ch == ' ') Game.HardDrop();
    if (Ch == 'x') Game.Turn(1);
    if (Ch == 'z') Game.Turn(-1);
    if (Ch == 'c') Game.Hold();
    return true;
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}

int main()
{
    try {
        Table Desk;
        std::expected<Stage, StageError> Opened = Stage::Open(64, 22);
        if (!Opened) {
            Say(Opened.error().Describe("tetris"));
            return 1;
        }
        Desk.Screen = std::move(*Opened);
        Desk.Measure();
        std::string Trouble;
        unsigned    Kept = ReadBest(Trouble);
        Desk.Best        = Kept;
        Desk.Game.Start(cpu_rdtsc());

        std::uint64_t Last    = clock_uptime_us();
        bool          Painted = true;
        for (;;) {
            std::uint64_t Now     = clock_uptime_us();
            float         Seconds = std::min(MostStep, static_cast<float>(Now - Last) / 1e6f);
            Last = Now;
            Desk.Clock += Seconds;
            if (!Desk.Paused) Desk.Game.Pass(Seconds);
            Desk.Best = std::max(Desk.Best, Desk.Game.Score);
            Desk.Draw();
            if (!(Painted = static_cast<bool>(Desk.Screen->Present()))) break;

            std::uint64_t Spent = clock_uptime_us() - Now;
            const painter::tui::Event Event = Desk.Screen->Wait(
                Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind == EventKind::Key && !OnKey(Desk, Event.Key)) break;
        }

        unsigned Score = Desk.Game.Score, Best = Desk.Best;
        (void)Desk.Screen->Close();
        if (Trouble.empty() && Best > Kept) {
            if (std::optional<std::string> Refusal = KeepWrite(Record, std::format("best {}\n", Best))) Trouble = *Refusal;
        }
        Say(std::format("[TETRIS] score {}, {} lines, best {}", Score, Desk.Game.Lines, Best));
        if (!Trouble.empty()) Say(std::format("tetris: the best score is not kept: {}", Trouble));
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for tetris.");
        return 1;
    }
}
