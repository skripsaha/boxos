/* 2048 — slide the tiles, join the equal ones, reach 2048.
 *
 *   arrows          slide every tile that way; two equal ones that meet become one
 *   u               take the last move back
 *   r               a new game
 *   q, ESC          leave; the best score is kept
 *
 * The tiles slide as they move and the joined ones flash. The best score is
 * kept on the volume, in 2048.record (keep.h), and outlives the machine. */

#include <algorithm>
#include <array>
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
#include "painter/tui/stage.h"

BOX_FORM("");

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

constexpr int           Side      = 4;
constexpr int           Goal      = 2048;
constexpr std::uint64_t SlideUs   = 110000;
constexpr std::uint64_t FlashUs   = 140000;
constexpr std::uint64_t FrameUs   = 16667;
const Keep              Record{"2048.record", "2048,record"};

/* A dark board, so the text console tells its places from it: the board and
 * the page black there, an empty place dark grey, the tiles light. */
const Color Page      = Color::Rgb(0x14, 0x12, 0x10);
const Color BoardInk  = Color::Rgb(0x2A, 0x26, 0x22);
const Color Empty     = Color::Rgb(0x4E, 0x48, 0x42);
const Color DarkText  = Color::Rgb(0x5A, 0x50, 0x48);
const Color LightText = Color::Rgb(0xF9, 0xF6, 0xF2);
const Color TitleInk  = Color::Rgb(0xED, 0xC2, 0x2E);
const Color HintInk   = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color KeyInk    = Colors::Amber;
const Color BarInk    = Color::Rgb(0xEE, 0xEE, 0xEE);

using Board = std::array<std::array<int, Side>, Side>;

Color TileInk(int Value)
{
    switch (Value) {
    case 2:    return Color::Rgb(0xEE, 0xE4, 0xDA);
    case 4:    return Color::Rgb(0xED, 0xE0, 0xC8);
    case 8:    return Color::Rgb(0xF2, 0xB1, 0x79);
    case 16:   return Color::Rgb(0xF5, 0x95, 0x63);
    case 32:   return Color::Rgb(0xF6, 0x7C, 0x5F);
    case 64:   return Color::Rgb(0xD8, 0x40, 0x1C);
    case 128:  return Color::Rgb(0xED, 0xCF, 0x72);
    case 256:  return Color::Rgb(0xED, 0xCC, 0x61);
    case 512:  return Color::Rgb(0xED, 0xC8, 0x50);
    case 1024: return Color::Rgb(0xED, 0xC5, 0x3F);
    case 2048: return Color::Rgb(0xED, 0xC2, 0x2E);
    default:   return Color::Rgb(0x3C, 0x3A, 0x32);
    }
}

Color TextOn(int Value)
{
    return Value <= 4 ? DarkText : LightText;
}

/* A tile on its way: from a place to a place, and whether it joins there. */
struct Slide {
    int  FromRow, FromCol, ToRow, ToCol, Value;
    bool Joins;
};

enum class Way { Left, Right, Up, Down };

class Game {
public:
    void New()
    {
        for (auto &Row : Tiles) Row.fill(0);
        Score = 0;
        Won = Kept = Ended = false;
        CanUndo = false;
        Place();
        Place();
    }

    /* The move made: the tiles slid, joined, one more placed. False when
     * nothing could move that way. */
    bool Move(Way Toward, std::vector<Slide> &Slides);
    void Undo()
    {
        if (!CanUndo) return;
        Tiles   = Before;
        Score   = ScoreBefore;
        CanUndo = false;
        Ended   = false;
    }
    bool Stuck() const;

    Board         Tiles{};
    int           Score = 0;
    int           Best  = 0;
    bool          Won   = false;   /* 2048 reached */
    bool          Kept  = false;   /* and the person went on */
    bool          Ended = false;
    int           NewRow = -1, NewCol = -1;

private:
    std::uint64_t Roll()
    {
        Seed ^= Seed << 13;
        Seed ^= Seed >> 7;
        Seed ^= Seed << 17;
        return Seed;
    }
    void Place();

    Board         Before{};
    int           ScoreBefore = 0;
    bool          CanUndo = false;
    std::uint64_t Seed = cpu_rdtsc() | 1;
};

void Game::Place()
{
    int Free = 0;
    for (auto &Row : Tiles)
        for (int Value : Row) Free += Value == 0;
    if (Free == 0) return;
    int Pick = static_cast<int>(Roll() % static_cast<std::uint64_t>(Free));
    for (int R = 0; R < Side; R++) {
        for (int C = 0; C < Side; C++) {
            if (Tiles[R][C] != 0 || Pick-- != 0) continue;
            Tiles[R][C] = Roll() % 10 == 0 ? 4 : 2;
            NewRow = R;
            NewCol = C;
            return;
        }
    }
}

bool Game::Stuck() const
{
    for (int R = 0; R < Side; R++) {
        for (int C = 0; C < Side; C++) {
            if (Tiles[R][C] == 0) return false;
            if (C + 1 < Side && Tiles[R][C] == Tiles[R][C + 1]) return false;
            if (R + 1 < Side && Tiles[R][C] == Tiles[R + 1][C]) return false;
        }
    }
    return true;
}

/* Every line is read from the side the tiles go to: the first tile goes to
 * the first place, one equal to the tile before it joins it — once. */
bool Game::Move(Way Toward, std::vector<Slide> &Slides)
{
    Board Was      = Tiles;
    int   WasScore = Score;
    Slides.clear();
    bool Moved = false;

    for (int Line = 0; Line < Side; Line++) {
        auto Place = [&](int At, int &Row, int &Col) {
            switch (Toward) {
            case Way::Left:  Row = Line; Col = At; break;
            case Way::Right: Row = Line; Col = Side - 1 - At; break;
            case Way::Up:    Row = At; Col = Line; break;
            case Way::Down:  Row = Side - 1 - At; Col = Line; break;
            }
        };
        std::array<int, Side>  Out{};
        std::array<bool, Side> Joined{};
        int                    Next = 0;
        for (int At = 0; At < Side; At++) {
            int R, C;
            Place(At, R, C);
            int Value = Was[R][C];
            if (Value == 0) continue;
            int ToR, ToC;
            if (Next > 0 && Out[Next - 1] == Value && !Joined[Next - 1]) {
                Out[Next - 1] *= 2;
                Joined[Next - 1] = true;
                Score += Out[Next - 1];
                Place(Next - 1, ToR, ToC);
                Slides.push_back({R, C, ToR, ToC, Value, true});
                Moved = true;
                continue;
            }
            Out[Next] = Value;
            Place(Next, ToR, ToC);
            Slides.push_back({R, C, ToR, ToC, Value, false});
            Moved = Moved || ToR != R || ToC != C;
            Next++;
        }
        for (int At = 0; At < Side; At++) {
            int R, C;
            Place(At, R, C);
            Tiles[R][C] = Out[At];
        }
    }
    if (!Moved) {
        Score = WasScore;
        return false;
    }
    Before      = Was;
    ScoreBefore = WasScore;
    CanUndo     = true;
    Best        = std::max(Best, Score);
    Place();
    for (auto &Row : Tiles)
        for (int Value : Row) Won = Won || Value >= Goal;
    Ended = Stuck();
    return true;
}

/* ---- the glass ----------------------------------------------------------- */

class Table {
public:
    std::optional<Stage> Screen;
    Game                Play;

    void Measure()
    {
        const painter::core::Size Dimensions = Screen->Size();
        TileHigh = std::max(3, (Dimensions.Height - 4 - (Side + 1)) / Side);
        TileWide = TileHigh * 2 + 3;
        while (TileWide > 3 && Side * TileWide + Side + 1 > Dimensions.Width - 2) TileWide--;
        BoardWide = Side * TileWide + Side + 1;
        BoardHigh = Side * TileHigh + Side + 1;
        Left = (Dimensions.Width - BoardWide) / 2;
        Top  = std::max(3, (Dimensions.Height - BoardHigh) / 2 + 1);
    }

    void Draw(const std::vector<Slide> &Slides, float Along, float Flash);
    void Overlay(std::string_view Line, std::string_view Hint);

private:
    void Tile(int Row, int Col, int Value, Color Ink, int Shrink);
    void Head();

    int TileWide = 11, TileHigh = 4, BoardWide = 0, BoardHigh = 0, Left = 0, Top = 0;
};

/* A tile at a place in cells, `Shrink` cells in from each side. */
void Table::Tile(int Row, int Col, int Value, Color Ink, int Shrink)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    int High = TileHigh - 2 * std::min(Shrink, TileHigh / 2);
    int Wide = TileWide - 4 * std::min(Shrink, TileHigh / 2);
    int Y    = Row + (TileHigh - High) / 2;
    int X    = Col + (TileWide - Wide) / 2;
    for (int R = 0; R < High; R++) FrameBuffer.Fill({X, Y + R, Wide, 1}, ' ', TextStyle{Ink, Ink});
    if (Value == 0 || High < 1) return;
    std::string Text = std::to_string(Value);
    if (static_cast<int>(Text.size()) > Wide) return;
    FrameBuffer.Write({X + (Wide - static_cast<int>(Text.size())) / 2, Y + High / 2}, Text,
                      TextStyle{TextOn(Value), Ink});
}

void Table::Head()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    FrameBuffer.Write({Left, 1}, "2048", TextStyle{TitleInk, Page});
    std::string Scores = std::format("score {}   best {}", Play.Score, Play.Best);
    FrameBuffer.Write({Left + BoardWide - static_cast<int>(Scores.size()), 1}, Scores, TextStyle{BarInk, Page});

    int Row = std::min(Dimensions.Height - 1, Top + BoardHigh), Col = Left;
    for (auto [Keys, What] : {std::pair{"\x1B\x1A\x18\x19", "slide"}, {"u", "undo"}, {"r", "new"}, {"q", "leave"}}) {
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Keys, TextStyle{KeyInk, Page}));
        Col += 1;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, What, TextStyle{HintInk, Page}));
        Col += 2;
    }
}

/* The board with every tile sliding `Along` of its way; a tile that joined
 * flashes while `Flash` lasts, and the new one grows in. */
void Table::Draw(const std::vector<Slide> &Slides, float Along, float Flash)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    FrameBuffer.Clear(TextStyle{Page, Page});
    Head();
    for (int R = 0; R < BoardHigh; R++)
        FrameBuffer.Fill({Left, Top + R, BoardWide, 1}, ' ', TextStyle{BoardInk, BoardInk});
    auto CellRow = [&](float R) { return Top + 1 + static_cast<int>(R * (TileHigh + 1) + 0.5f); };
    auto CellCol = [&](float C) { return Left + 1 + static_cast<int>(C * (TileWide + 1) + 0.5f); };

    for (int R = 0; R < Side; R++)
        for (int C = 0; C < Side; C++) Tile(CellRow(static_cast<float>(R)), CellCol(static_cast<float>(C)), 0, Empty, 0);

    if (Along < 1.0f) {
        for (const Slide &One : Slides) {
            float R = One.FromRow + (One.ToRow - One.FromRow) * Along;
            float C = One.FromCol + (One.ToCol - One.FromCol) * Along;
            Tile(CellRow(R), CellCol(C), One.Value, TileInk(One.Value), 0);
        }
        return;
    }
    for (int R = 0; R < Side; R++) {
        for (int C = 0; C < Side; C++) {
            int Value = Play.Tiles[R][C];
            if (Value == 0) continue;
            bool Joined = false;
            for (const Slide &One : Slides) Joined = Joined || (One.Joins && One.ToRow == R && One.ToCol == C);
            bool  Fresh = R == Play.NewRow && C == Play.NewCol && Flash > 0;
            Color Ink = Joined && Flash > 0 ? Color::Mix(TileInk(Value), Colors::White,
                                                         static_cast<std::uint8_t>(110 * Flash))
                                            : TileInk(Value);
            Tile(CellRow(static_cast<float>(R)), CellCol(static_cast<float>(C)), Value, Ink, Fresh ? static_cast<int>(Flash * 2 + 0.5f) : 0);
        }
    }
}

void Table::Overlay(std::string_view Line, std::string_view Hint)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    int Wide = static_cast<int>(std::max(Line.size(), Hint.size())) + 6;
    int Row  = Top + BoardHigh / 2 - 2, Col = Left + (BoardWide - Wide) / 2;
    const Color Overlay = Color::Rgb(0x3C, 0x3A, 0x32);
    for (int R = 0; R < 5; R++) FrameBuffer.Fill({Col, Row + R, Wide, 1}, ' ', TextStyle{LightText, Overlay});
    FrameBuffer.Write({Col + (Wide - static_cast<int>(Line.size())) / 2, Row + 1}, Line,
                      TextStyle{TitleInk, Overlay});
    FrameBuffer.Write({Col + (Wide - static_cast<int>(Hint.size())) / 2, Row + 3}, Hint,
                      TextStyle{LightText, Overlay});
}

int ReadBest(std::string &Trouble)
{
    std::string Text;
    if (std::optional<std::string> Refusal = KeepRead(Record, Text)) {
        Trouble = *Refusal;
        return 0;
    }
    int Best = 0;
    std::size_t At = Text.find("best ");
    if (At != std::string::npos) std::from_chars(Text.data() + At + 5, Text.data() + Text.size(), Best);
    return Best;
}

enum class Through { Done, Interrupted, Gone };

/* The board drawn through a move: the slide, then the flash, a frame at a
 * time with the keys heard between. A key cuts it short — the board is where
 * the move left it already — and is handed back to be acted on. */
Through Animate(Table &Desk, const std::vector<Slide> &Slides, KeyEvent &Key)
{
    std::uint64_t Began = clock_uptime_us();
    for (;;) {
        std::uint64_t Frame = clock_uptime_us();
        std::uint64_t Gone  = Frame - Began;
        if (Gone >= SlideUs + FlashUs) return Through::Done;
        float Along = std::min(1.0f, static_cast<float>(Gone) / SlideUs);
        float Flash = Gone < SlideUs ? 1.0f : 1.0f - static_cast<float>(Gone - SlideUs) / FlashUs;
        Desk.Draw(Slides, Along, Flash);
        if (!Desk.Screen->Present()) return Through::Gone;

        std::uint64_t Spent = clock_uptime_us() - Frame;
        const painter::tui::Event Event = Desk.Screen->Wait(
            Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
        if (Event.Kind == EventKind::Gone) return Through::Gone;
        if (Event.Kind == EventKind::Key) {
            Key = Event.Key;
            return Through::Interrupted;
        }
    }
}

}

int main()
{
    try {
        Table Desk;
        std::expected<Stage, StageError> Opened = Stage::Open(40, 20);
        if (!Opened) {
            const std::string Message = Opened.error().Describe("2048");
            print_to_screen((Message + '\n').c_str());
            return 1;
        }
        Desk.Screen = std::move(*Opened);
        Desk.Measure();
        std::string Trouble;
        int         Kept = ReadBest(Trouble);
        Desk.Play.Best   = Kept;
        Desk.Play.New();

        std::vector<Slide> Slides;
        bool               Painted = true, Waiting = false;
        KeyEvent            Key;
        for (;;) {
            if (!Waiting) {
                Desk.Draw(Slides, 1.0f, 0.0f);
                if (Desk.Play.Won && !Desk.Play.Kept) Desk.Overlay("2048!", "c to keep going, r for a new game");
                else if (Desk.Play.Ended) Desk.Overlay("no move left", "u to take one back, r for a new game");
                if (!(Painted = static_cast<bool>(Desk.Screen->Present()))) break;
                const painter::tui::Event Event = Desk.Screen->Wait();
                if (Event.Kind == EventKind::Gone) break;
                if (Event.Kind != EventKind::Key) continue;
                Key = Event.Key;
            }
            Waiting = false;

            if (Key.IsExtended()) {
                if (Desk.Play.Ended || (Desk.Play.Won && !Desk.Play.Kept)) continue;
                Way Toward;
                switch (Key.Scancode) {
                case painter::tui::KeyCodes::Left:  Toward = Way::Left; break;
                case painter::tui::KeyCodes::Right: Toward = Way::Right; break;
                case painter::tui::KeyCodes::Up:    Toward = Way::Up; break;
                case painter::tui::KeyCodes::Down:  Toward = Way::Down; break;
                default: continue;
                }
                if (!Desk.Play.Move(Toward, Slides)) continue;
                Through End = Animate(Desk, Slides, Key);
                if (End == Through::Gone) {
                    Painted = false;
                    break;
                }
                Waiting = End == Through::Interrupted;
                continue;
            }
            char Ch = Key.Ascii;
            if (Ch == 'q' || Ch == 'Q' || Ch == painter::tui::KeyCodes::Escape) break;
            if (Ch == 'u' || Ch == 'U') { Desk.Play.Undo(); Slides.clear(); }
            if (Ch == 'r' || Ch == 'R') { Desk.Play.New(); Slides.clear(); }
            if ((Ch == 'c' || Ch == 'C') && Desk.Play.Won) Desk.Play.Kept = true;
        }

        int Score = Desk.Play.Score, Best = Desk.Play.Best;
        (void)Desk.Screen->Close();
        /* Kept only when beaten, and never over a record that could not be read. */
        if (Trouble.empty() && Best > Kept) {
            if (std::optional<std::string> Refusal = KeepWrite(Record, std::format("best {}\n", Best))) Trouble = *Refusal;
        }
        const std::string Message = std::format("[2048] score {}, best {}", Score, Best);
        print_to_screen((Message + '\n').c_str());
        if (!Trouble.empty()) {
            const std::string TroubleMessage = std::format("2048: the best score is not kept: {}", Trouble);
            print_to_screen((TroubleMessage + '\n').c_str());
        }
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        print_to_screen("There is not enough memory for 2048.\n");
        return 1;
    }
}
