/* minesweeper — find the mines without stepping on one.
 *
 *   minesweeper              the intermediate field, 16 x 16 with 40 mines
 *   minesweeper expert       30 x 16 with 99; or beginner, 9 x 9 with 10
 *
 *   arrows        move                  space, enter  open; on a number, chord
 *   f             flag, or unflag       1 2 3         beginner, intermediate, expert
 *   r             a new field           q, ESC        leave
 *
 * The rules are minesweeper_field.cpp. The squares are as big as the glass
 * allows — two or three cells wide on a text console, up to six by three on a wide
 * framebuffer — raised while closed and sunk once open. The best time of
 * each field is kept in minesweeper.record (keep.h). */

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <new>
#include <string>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "keep.h"
#include "minesweeper.h"
#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/stage.h"

BOX_FORM("takes optional words \"the field: beginner, intermediate or expert\"\n");

namespace {

using Color = painter::core::Color;
namespace Colors = painter::core::Colors;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

const Keep              Record{"minesweeper.record", "minesweeper,record"};
constexpr std::uint64_t RevealUs = 30000;   /* between two mines shown after a loss */

const Color Page        = Color::Rgb(0x12, 0x12, 0x12);
/* A big square stands apart by its gaps; a square one row high has none to
 * spare, so its neighbours alternate light and dark grey — two greys a text
 * console tells apart. The one chosen is white. */
const Color Raised      = Color::Rgb(0x9A, 0x9A, 0x9A);
const Color RaisedOdd   = Color::Rgb(0xA8, 0xA8, 0xA8);
const Color RaisedEven  = Color::Rgb(0x78, 0x78, 0x78);
const Color RaisedHere  = Color::Rgb(0xE8, 0xE8, 0xE8);
const Color RaisedLight = Color::Rgb(0xC8, 0xC8, 0xC8);
const Color Sunken      = Color::Rgb(0x2A, 0x2A, 0x2A);
const Color SunkenHere  = Color::Rgb(0x4A, 0x4A, 0x4A);
const Color Blast       = Color::Rgb(0xC0, 0x30, 0x30);
const Color FlagInk     = Color::Rgb(0xE0, 0x30, 0x30);
const Color MineInk     = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color TitleInk    = Colors::Amber;
const Color TextInk     = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color HintInk     = Color::Rgb(0x8C, 0x8C, 0x8C);
const Color GoodInk     = Color::Rgb(0x60, 0xE0, 0x70);
const Color BadInk      = Color::Rgb(0xFF, 0x60, 0x60);
const Color FaceInk     = Color::Rgb(0xF0, 0xD0, 0x40);
const Color BarPaper    = Color::Rgb(0x3A, 0x3A, 0x3A);
const Color KeyInk      = Colors::Amber;

/* The numbers as the game has always had them, lightened for a dark square:
 * blue, green, red, violet, amber, cyan, white, grey. */
const Color NumberInks[9] = {Sunken,
                             Color::Rgb(0x60, 0x90, 0xFF), Color::Rgb(0x50, 0xC8, 0x50), Color::Rgb(0xFF, 0x50, 0x50),
                             Color::Rgb(0xC0, 0x70, 0xFF), Color::Rgb(0xE0, 0xA0, 0x30), Color::Rgb(0x40, 0xD0, 0xD0),
                             Color::Rgb(0xEE, 0xEE, 0xEE), Color::Rgb(0x9A, 0x9A, 0x9A)};

constexpr char FlagGlyph = painter::tui::Glyphs::Pointer;
constexpr char MineGlyph = static_cast<char>(0x0F);

/* Square sizes, biggest first: the first the glass holds is taken. */
constexpr int TileSizes[][2] = {{6, 3}, {4, 2}, {3, 1}, {2, 1}};

class Board {
public:
    std::optional<Stage> Screen;
    MineField            Field;
    int       Played = 0, Cleared = 0;
    int       Best[MineSizeCount] = {};
    std::string Trouble;

    void Begin(int Which);
    bool Fits(int Which) const;
    void Draw();
    /* False to leave. */
    bool OnKey(const KeyEvent &Key);
    std::uint32_t Wait() const;
    int  Level() const { return Which; }
    bool Beaten() const { return NewBest; }

private:
    void Tile(int X, int Y, const std::vector<int> &Rank, int ShownMines);
    void Head();
    void Foot();
    void Act(bool Flag);
    int  Seconds() const;

    int           Which = 1;
    int           CursorX = 0, CursorY = 0;
    int           TileW = 2, TileH = 1, Left = 0, Top = 0;
    std::uint64_t StartedAt = 0, EndedAt = 0;
    bool          NewBest = false;
};

bool Board::Fits(int Level) const
{
    const painter::core::Size Dimensions = Screen->Size();
    const MineSize &Size = MineSizes[Level];
    return Size.Wide * 2 <= Dimensions.Width - 2 && Size.High <= Dimensions.Height - 5;
}

void Board::Begin(int Level)
{
    Which = Level;
    const MineSize &Size = MineSizes[Which];
    Field.Start(Size, cpu_rdtsc());
    const painter::core::Size Dimensions = Screen->Size();
    for (const auto &Tile : TileSizes) {
        TileW = Tile[0];
        TileH = Tile[1];
        if (Size.Wide * TileW <= Dimensions.Width - 2 && Size.High * TileH <= Dimensions.Height - 5) break;
    }
    Left      = (Dimensions.Width - Size.Wide * TileW) / 2;
    Top       = std::max(2, (Dimensions.Height - 1 - Size.High * TileH) / 2 + 1);
    CursorX   = Size.Wide / 2;
    CursorY   = Size.High / 2;
    StartedAt = EndedAt = 0;
    NewBest   = false;
}

int Board::Seconds() const
{
    if (StartedAt == 0) return 0;
    std::uint64_t End = EndedAt ? EndedAt : clock_uptime_us();
    return std::min(999, static_cast<int>((End - StartedAt) / 1000000));
}

/* One square: raised and closed, a flag on it, sunk and open with its number,
 * or a mine shown once the field is lost. */
void Board::Tile(int X, int Y, const std::vector<int> &Rank, int ShownMines)
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const MineCell &Cell = Field.At(X, Y);
    bool  Here   = X == CursorX && Y == CursorY;
    bool  Lost   = Field.State == MineState::Lost;
    bool  Shown  = Cell.Open || (Lost && Cell.Mine && Rank[static_cast<std::size_t>(Y * Field.Wide + X)] < ShownMines);
    int   Row    = Top + Y * TileH, Col = Left + X * TileW;
    char  Mark  = ' ';
    Color Ink    = TextInk;
    Color Paper;

    if (!Shown) {
        Paper = Here ? RaisedHere : TileH > 1 ? Raised : (X + Y) % 2 ? RaisedOdd : RaisedEven;
        if (Cell.Flag) {
            bool Wrong = Lost && !Cell.Mine;
            Mark = Wrong ? 'X' : FlagGlyph;
            Ink   = Wrong ? Blast : FlagInk;
        }
    } else {
        Paper = Here ? SunkenHere : Sunken;
        if (Cell.Mine) {
            Mark = MineGlyph;
            Ink   = MineInk;
            if (X == Field.HitX && Y == Field.HitY) Paper = Blast;
        } else if (Cell.Near) {
            Mark = static_cast<char>('0' + Cell.Near);
            Ink   = NumberInks[Cell.Near];
        }
    }
    /* A half-cell gap along the bottom and the right of a square big enough
     * to spare it, so the squares stand apart; a raised one lit along its top. */
    bool GapBelow = TileH > 1, GapRight = TileH > 1;
    for (int R = 0; R < TileH; R++) {
        for (int C = 0; C < TileW; C++) {
            bool Below = GapBelow && R == TileH - 1, Right = GapRight && C == TileW - 1;
            if (Below && Right) FrameBuffer.Put({Col + C, Row + R}, ' ', TextStyle{Page, Page});
            else if (Right)
                FrameBuffer.Put({Col + C, Row + R}, painter::tui::Glyphs::LeftHalf, TextStyle{Paper, Page});
            else if (Below)
                FrameBuffer.Put({Col + C, Row + R}, painter::tui::Glyphs::LowerHalf, TextStyle{Page, Paper});
            else if (R == 0 && TileH > 1 && !Shown)
                FrameBuffer.Put({Col + C, Row + R}, painter::tui::Glyphs::UpperHalf,
                                TextStyle{RaisedLight, Paper});
            else FrameBuffer.Put({Col + C, Row + R}, ' ', TextStyle{Ink, Paper});
        }
    }
    if (Mark != ' ')
        FrameBuffer.Put({Col + (TileW - 1) / 2, Row + (TileH - 1) / 2}, Mark, TextStyle{Ink, Paper});
}

/* The flags left, a face, the time. */
void Board::Head()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    int Wide  = std::max(Field.Wide * TileW, 30);
    int Start = (Dimensions.Width - Wide) / 2;
    FrameBuffer.Write({Start, Top - 2}, "minesweeper", TextStyle{TitleInk, Page});
    std::string Name = MineSizes[Which].Name;
    FrameBuffer.Write({Start + Wide - static_cast<int>(Name.size()), Top - 2}, Name, TextStyle{HintInk, Page});

    FrameBuffer.Put({Start, Top - 1}, FlagGlyph, TextStyle{FlagInk, Page});
    FrameBuffer.Write({Start + 2, Top - 1}, std::format("{:>3}", Field.FlagsLeft()), TextStyle{TextInk, Page});
    const char *Face = Field.State == MineState::Won ? "B)" : Field.State == MineState::Lost ? "X(" : ":)";
    FrameBuffer.Write({Start + Wide / 2 - 1, Top - 1}, Face,
                      TextStyle{Field.State == MineState::Lost ? BadInk : FaceInk, Page});
    std::string Time = std::format("{:03} s", Seconds());
    FrameBuffer.Write({Start + Wide - static_cast<int>(Time.size()), Top - 1}, Time, TextStyle{TextInk, Page});
}

/* How the field went, under it, and the keys along the bottom. */
void Board::Foot()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    int Row = Top + Field.High * TileH;
    if (Row < Dimensions.Height - 1) {
        int BestTime = Best[Which];
        if (Field.State == MineState::Won)
            FrameBuffer.Centre(Row, NewBest ? std::format("cleared in {} s, a new best!", Seconds())
                                            : std::format("cleared in {} s (best {} s)", Seconds(), BestTime),
                               TextStyle{GoodInk, Page});
        else if (Field.State == MineState::Lost)
            FrameBuffer.Centre(Row, "boom. r for a new field", TextStyle{BadInk, Page});
        else if (BestTime > 0)
            FrameBuffer.Centre(Row, std::format("best {} s", BestTime), TextStyle{HintInk, Page});
    }
    int Bar = Dimensions.Height - 1, Col = 1;
    FrameBuffer.Fill({0, Bar, Dimensions.Width, 1}, ' ', TextStyle{TextInk, BarPaper});
    for (auto [Keys, What] : {std::pair{"\x1B\x18\x19\x1A", "move"}, {"space", "open"}, {"f", "flag"},
                              {"1 2 3", "field"}, {"r", "new"}, {"q", "leave"}}) {
        int Wide = static_cast<int>(std::string_view(Keys).size() + std::string_view(What).size()) + 3;
        if (Col + Wide > Dimensions.Width) break;
        Col += static_cast<int>(FrameBuffer.Write({Col, Bar}, Keys, TextStyle{KeyInk, BarPaper}));
        Col += 1;
        Col += static_cast<int>(FrameBuffer.Write({Col, Bar}, What, TextStyle{HintInk, BarPaper}));
        Col += 2;
    }
}

void Board::Draw()
{
    Screen->FrameBuffer().Clear(TextStyle{Page, Page});
    std::vector<int> Order = Field.MinesByDistance();
    std::vector<int> Rank(static_cast<std::size_t>(Field.Wide * Field.High), 1 << 30);
    for (std::size_t At = 0; At < Order.size(); At++) Rank[static_cast<std::size_t>(Order[At])] = static_cast<int>(At);
    int Shown = 0;
    if (Field.State == MineState::Lost)
        Shown = static_cast<int>(std::min<std::uint64_t>(Order.size(), (clock_uptime_us() - EndedAt) / RevealUs + 1));

    Head();
    for (int Y = 0; Y < Field.High; Y++)
        for (int X = 0; X < Field.Wide; X++) Tile(X, Y, Rank, Shown);
    Foot();
}

/* An opening or a flag, and what it changed: the clock started, stopped, a
 * best time kept. */
void Board::Act(bool Flag)
{
    MineState Was = Field.State;
    if (Flag) Field.ToggleFlag(CursorX, CursorY);
    else if (Field.At(CursorX, CursorY).Open) Field.Chord(CursorX, CursorY);
    else Field.Open(CursorX, CursorY);

    std::uint64_t Now = clock_uptime_us();
    if (Was == MineState::Fresh && Field.State != MineState::Fresh) {
        StartedAt = Now;
        Played++;
    }
    if (Was == Field.State || (Field.State != MineState::Won && Field.State != MineState::Lost)) return;
    EndedAt = Now;
    if (Field.State != MineState::Won) return;
    Cleared++;
    int Time = std::max(1, Seconds());
    if (Best[Which] == 0 || Time < Best[Which]) {
        Best[Which] = Time;
        NewBest     = true;
    }
}

bool Board::OnKey(const KeyEvent &Key)
{
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  CursorX = std::max(0, CursorX - 1); break;
        case painter::tui::KeyCodes::Right: CursorX = std::min(Field.Wide - 1, CursorX + 1); break;
        case painter::tui::KeyCodes::Up:    CursorY = std::max(0, CursorY - 1); break;
        case painter::tui::KeyCodes::Down:  CursorY = std::min(Field.High - 1, CursorY + 1); break;
        default: break;
        }
        return true;
    }
    char Ch = Key.Ascii;
    if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    switch (Ch) {
    case 'q':
    case painter::tui::KeyCodes::Escape: return false;
    case ' ':
    case '\n':
    case '\r': Act(false); break;
    case 'f':  Act(true); break;
    case 'r':  Begin(Which); break;
    case '1':
    case '2':
    case '3':
        if (Fits(Ch - '1')) Begin(Ch - '1');
        break;
    default: break;
    }
    return true;
}

/* While the clock runs, until its next second; while the mines are being
 * shown, the next of them; else the keys alone. */
std::uint32_t Board::Wait() const
{
    std::uint64_t Now = clock_uptime_us();
    if (Field.State == MineState::Lost && (Now - EndedAt) / RevealUs < Field.MinesByDistance().size())
        return static_cast<std::uint32_t>(RevealUs / 1000);
    if (Field.State == MineState::Playing)
        return static_cast<std::uint32_t>((1000000 - (Now - StartedAt) % 1000000) / 1000 + 1);
    return 0;
}

void ReadBest(Board &Desk)
{
    std::string Text;
    if (std::optional<std::string> Refusal = KeepRead(Record, Text)) {
        Desk.Trouble = *Refusal;
        return;
    }
    for (int Level = 0; Level < MineSizeCount; Level++) {
        std::string Name = std::string(MineSizes[Level].Name) + ' ';
        std::size_t At   = Text.find(Name);
        if (At != std::string::npos)
            std::from_chars(Text.data() + At + Name.size(), Text.data() + Text.size(), Desk.Best[Level]);
    }
}

std::string BestText(const Board &Desk)
{
    std::string Text;
    for (int Level = 0; Level < MineSizeCount; Level++)
        if (Desk.Best[Level] > 0) Text += std::format("{} {}\n", MineSizes[Level].Name, Desk.Best[Level]);
    return Text;
}

int Asked(const char *const *Words, std::size_t Count)
{
    for (std::size_t At = 0; At < Count; At++)
        for (int Level = 0; Level < MineSizeCount; Level++)
            if (std::strcmp(Words[At], MineSizes[Level].Name) == 0) return Level;
    return -1;
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
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        Board Desk;
        std::expected<Stage, StageError> Opened = Stage::Open(40, 16);
        if (!Opened) {
            Say(Opened.error().Describe("minesweeper"));
            return 1;
        }
        Desk.Screen = std::move(*Opened);
        int Level = Asked(Words, Count);
        if (Level < 0) Level = Desk.Fits(1) ? 1 : 0;
        if (!Desk.Fits(Level)) Level = 0;
        ReadBest(Desk);
        int Kept[MineSizeCount];
        std::copy(std::begin(Desk.Best), std::end(Desk.Best), Kept);
        Desk.Begin(Level);

        bool Painted = true;
        for (;;) {
            Desk.Draw();
            if (!(Painted = static_cast<bool>(Desk.Screen->Present()))) break;
            const painter::tui::Event Event = Desk.Screen->Wait(Desk.Wait());
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind == EventKind::Key && !Desk.OnKey(Event.Key)) break;
        }
        (void)Desk.Screen->Close();

        /* Kept only when beaten, and never over a record that could not be read. */
        if (Desk.Trouble.empty() && !std::equal(std::begin(Kept), std::end(Kept), std::begin(Desk.Best))) {
            if (std::optional<std::string> Refusal = KeepWrite(Record, BestText(Desk))) Desk.Trouble = *Refusal;
        }
        Say(std::format("[MINESWEEPER] {} played, {} cleared", Desk.Played, Desk.Cleared));
        if (!Desk.Trouble.empty()) Say("minesweeper: the best times are not kept: " + Desk.Trouble);
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for the field.");
        return 1;
    }
}
