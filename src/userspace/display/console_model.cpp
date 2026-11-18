#include "console_model.h"

#include <algorithm>
#include <cstring>

#include "box/color.h"
#include "boxos_log_tone.h"
#include "boxos_utf8.h"

namespace {

// What a cell is given for Codepoint: the character, or — for a control the
// console does not act on, which shows nothing a reader could take for text —
// U+FFFD, the character that says one is missing.
char32_t AsCell(std::uint32_t Codepoint) noexcept
{
    const bool Control = Codepoint < 0x20u || (Codepoint >= 0x7Fu && Codepoint < 0xA0u);
    return Control ? U'\uFFFD' : static_cast<char32_t>(Codepoint);
}

// The console's cell of a character in its colours: no mark.
ItemCell CellOf(char32_t Character, std::uint32_t Foreground, std::uint32_t Background) noexcept
{
    return ItemCell{static_cast<std::uint32_t>(Character), Foreground, Background, 0, ITEM_MARK_NONE, {0, 0, 0}};
}

}  // namespace

ConsoleModel::ConsoleModel(CellGrid &Surface) : Grid(Surface)
{
    // A line is never longer than the console: lifting one never allocates,
    // so no frame drawn while a line is typed can fail for want of memory.
    Lifted.reserve(Grid.Count());
    Clear(COLOR_DEFAULT, COLOR_DEFAULT);
}

void ConsoleModel::Clear(std::uint32_t Foreground, std::uint32_t Background)
{
    Grid.Fill(0, Grid.Count(), CellOf(U' ', Foreground, Background));
    CursorColumn = 0;
    CursorRow = 0;
    LineStart = LineEnd = Others = 0;
    Held = false;
    // Everything the console held is gone, and what stood in its flow with it.
    Gone += Height();
}

void ConsoleModel::Scroll()
{
    if (Height() == 0 || Width() == 0) return;
    Grid.Scroll(1, CellOf(U' ', COLOR_DEFAULT, COLOR_DEFAULT));
    Gone++;
    // What is kept as places on the console moves up with it.
    for (std::size_t *Place : {&LineStart, &LineEnd, &Others})
        *Place = *Place >= Width() ? *Place - Width() : 0;
}

void ConsoleModel::Newline()
{
    CursorColumn = 0;
    CursorRow++;
    if (CursorRow >= Height()) {
        Scroll();
        CursorRow = Height() == 0 ? 0 : Height() - 1u;
    }
}

void ConsoleModel::CarriageReturn()
{
    CursorColumn = 0;
}

void ConsoleModel::Backspace(std::uint32_t Foreground, std::uint32_t Background)
{
    if (CursorColumn == 0 || CursorRow >= Height()) return;
    CursorColumn--;
    Grid.Put(static_cast<std::size_t>(CursorRow) * Width() + CursorColumn, CellOf(U' ', Foreground, Background));
}

void ConsoleModel::Tab()
{
    const std::uint32_t Target = (CursorColumn + 8u) & ~7u;
    if (Target >= Width()) Newline();
    else CursorColumn = Target;
}

void ConsoleModel::WriteCell(char32_t Character, std::uint32_t Foreground, std::uint32_t Background)
{
    if (CursorRow < Height() && CursorColumn < Width())
        Grid.Put(static_cast<std::size_t>(CursorRow) * Width() + CursorColumn, CellOf(Character, Foreground, Background));
    CursorColumn++;
    if (CursorColumn >= Width()) Newline();
    LineEnd = std::max(LineEnd, Here());
}

void ConsoleModel::Write(const char *Bytes, std::size_t Length,
                         std::uint32_t Foreground, std::uint32_t Background)
{
    std::size_t Offset = 0;
    while (Offset < Length) {
        const char Byte = Bytes[Offset];
        if (static_cast<std::uint8_t>(Byte) < 0x80u) {
            switch (Byte) {
            case '\n':
                // The line is done: a new one begins, and nothing is held
                // under another's text any more.
                Newline();
                LineStart = LineEnd = Here();
                Held      = false;
                break;
            case '\r': CarriageReturn(); break;
            case '\b': Backspace(Foreground, Background); break;
            case '\t': Tab(); break;
            default:   WriteCell(AsCell(static_cast<std::uint8_t>(Byte)), Foreground, Background); break;
            }
            Offset++;
            continue;
        }

        // A byte that begins no character is one missing character: U+FFFD,
        // and the next byte read afresh (BoxUtf8Next).
        std::uint32_t Codepoint = 0;
        Offset += BoxUtf8Next(Bytes + Offset, Length - Offset, &Codepoint);
        WriteCell(AsCell(Codepoint), Foreground, Background);
    }
}

namespace {

// What each tone of the kernel's lines looks like on this console. The kernel
// says what kind of text a line is; the colour is the display's — the plain
// ones in the screen's text colour, every one on its page.
struct ToneColours {
    std::uint32_t Foreground;
    std::uint32_t Background;
};

constexpr ToneColours ToneTheme[LOG_TONE_COUNT] = {
    {COLOR_DEFAULT, COLOR_DEFAULT},   // PLAIN
    {COLOR_RED,     COLOR_DEFAULT},   // ERROR
    {COLOR_GREEN,   COLOR_DEFAULT},   // SUCCESS
    {COLOR_YELLOW,  COLOR_DEFAULT},   // WARNING
    {COLOR_CYAN,    COLOR_DEFAULT},   // HINT
    {COLOR_DEFAULT, COLOR_DEFAULT},   // SAID — a program's own word
};

const ToneColours &Theme(std::uint8_t Tone) noexcept
{
    return ToneTheme[Tone < LOG_TONE_COUNT ? Tone : LOG_TONE_PLAIN];
}

}  // namespace

void ConsoleModel::WriteKernel(const char *Bytes, const std::uint8_t *Tones, std::size_t Count)
{
    const auto Lines = [&] {
        std::size_t First = 0;
        while (First < Count) {
            std::size_t End = First + 1;
            while (End < Count && Tones[End] == Tones[First]) End++;
            const ToneColours &Colours = Theme(Tones[First]);
            Write(Bytes + First, End - First, Colours.Foreground, Colours.Background);
            First = End;
        }
    };
    if (Holds(0)) {
        AsAnother(0, true, Lines);
        return;
    }
    Begin(0, true);
    Lines();
}

void ConsoleModel::Step(std::int32_t Delta)
{
    if (Width() == 0 || Height() == 0) return;
    std::int64_t Linear = static_cast<std::int64_t>(CursorRow) * Width() + CursorColumn;
    Linear += Delta;
    const std::int64_t Last = static_cast<std::int64_t>(Width()) * Height() - 1;
    Linear = std::clamp<std::int64_t>(Linear, 0, Last);
    CursorRow = static_cast<std::uint32_t>(Linear / Width());
    CursorColumn = static_cast<std::uint32_t>(Linear % Width());
}

std::size_t ConsoleModel::Here() const noexcept
{
    return static_cast<std::size_t>(CursorRow) * Width() + CursorColumn;
}

void ConsoleModel::MoveTo(std::size_t Linear) noexcept
{
    if (Width() == 0 || Height() == 0) return;
    Linear       = std::min(Linear, Grid.Count() - 1u);
    CursorRow    = static_cast<std::uint32_t>(Linear / Width());
    CursorColumn = static_cast<std::uint32_t>(Linear % Width());
}

// One cell laid as it was, the caret after it — at the end of a row on the
// next, the console scrolled when that is past its last.
void ConsoleModel::Lay(const ItemCell &Cell)
{
    if (CursorRow < Height() && CursorColumn < Width())
        Grid.Put(static_cast<std::size_t>(CursorRow) * Width() + CursorColumn, Cell);
    CursorColumn++;
    if (CursorColumn >= Width()) Newline();
}

// Whether text of the lane `Lane` (0: the kernel's, or a fold) must go above
// the typing lane's unfinished line rather than after it.
bool ConsoleModel::Holds(std::uint32_t Lane) const noexcept
{
    return TypingLane != 0 && LastLane == TypingLane && Lane != TypingLane && LineEnd > LineStart;
}

// Another lane's text after the last lane's: its line begins where the caret
// is — on a row of its own when the last left its row unfinished, so no two
// lanes' words share a row. A fold (Text false) lies where it was asked to.
void ConsoleModel::Begin(std::uint32_t Lane, bool Text) noexcept
{
    const bool Another = Lane != LastLane;
    if (Lane != 0 && !Another) return;
    if (Text && Another && CursorColumn != 0) Newline();
    LastLane  = Lane;
    Held      = false;
    LineStart = LineEnd = Here();
}

// What another says while a line is typed: the line lifted off the console,
// the other text laid where the line stood — or, once held, where the other
// text left off — and the line laid back under it, on a row of its own when
// that text left a row unfinished, the caret where it stood in the line.
template <typename Drawing>
void ConsoleModel::AsAnother(std::uint32_t Lane, bool Text, Drawing &&Said)
{
    const std::size_t End = std::min(LineEnd, Grid.Count());
    Grid.CopyOut(LineStart, End, Lifted);
    const std::size_t Caret = std::min(Here() >= LineStart ? Here() - LineStart : 0, Lifted.size());
    Grid.Fill(LineStart, End, CellOf(U' ', COLOR_DEFAULT, COLOR_DEFAULT));

    MoveTo(Held ? Others : LineStart);
    if (Held && Text && Lane != OthersLane && CursorColumn != 0) Newline();
    Said();
    Others     = Here();
    OthersLane = Lane;
    Held       = true;

    if (CursorColumn != 0) Newline();
    LineStart = Here();
    for (const ItemCell &Cell : Lifted) Lay(Cell);
    LineEnd  = Here();
    LastLane = TypingLane;
    MoveTo(LineStart + Caret);
}

void ConsoleModel::Away(std::uint32_t Lane)
{
    Forget(Lane);
    if (TypingLane == Lane) TypingLane = 0;   // its line stays where it is, held under nothing
    if (Lane == 0 || LastLane != Lane || LineEnd <= LineStart) return;
    const std::size_t End = std::min(LineEnd, Grid.Count());
    Kept              One;
    One.Lane = Lane;
    Grid.CopyOut(LineStart, End, One.Line);
    One.Caret = std::min(Here() >= LineStart ? Here() - LineStart : 0, One.Line.size());
    KeptLines.push_back(std::move(One));
}

void ConsoleModel::Back(std::uint32_t Lane)
{
    const auto It =
        std::find_if(KeptLines.begin(), KeptLines.end(), [&](const Kept &One) { return One.Lane == Lane; });
    if (It == KeptLines.end()) return;
    const Kept One = std::move(*It);
    KeptLines.erase(It);

    if (CursorColumn != 0) Newline();
    LineStart = Here();
    for (const ItemCell &Cell : One.Line) Lay(Cell);
    LineEnd    = Here();
    LastLane   = Lane;
    TypingLane = Lane;
    Held       = false;
    MoveTo(LineStart + One.Caret);
}

void ConsoleModel::Forget(std::uint32_t Lane) noexcept
{
    std::erase_if(KeptLines, [&](const Kept &One) { return One.Lane == Lane; });
}

void ConsoleModel::Apply(const ConsoleRun &Run, std::uint32_t Lane)
{
    const bool Text = Run.kind == CONSOLE_RUN_TEXT || Run.kind == CONSOLE_RUN_HINT;
    if (Holds(Lane)) {
        AsAnother(Lane, Text, [&] { Draw(Run); });
        return;
    }
    Begin(Lane, Text);
    Draw(Run);
}

void ConsoleModel::Draw(const ConsoleRun &Run)
{
    switch (Run.kind) {
    case CONSOLE_RUN_CLEAR:
        Clear(BoxColorOnWire(Run.fg), BoxColorOnWire(Run.bg));
        return;
    case CONSOLE_RUN_STEP: {
        std::int32_t Delta = 0;
        if (Run.len >= sizeof(Delta)) std::memcpy(&Delta, Run.text, sizeof(Delta));
        Step(Delta);
        return;
    }
    case CONSOLE_RUN_TEXT:
    case CONSOLE_RUN_HINT:
        Write(Run.text, std::min<std::size_t>(Run.len, CONSOLE_RUN_TEXT_MAX), BoxColorOnWire(Run.fg), BoxColorOnWire(Run.bg));
        return;
    default:
        return;
    }
}

std::uint64_t ConsoleModel::Reserve(std::uint32_t Rows)
{
    const auto Room = [&] { Reserved(Rows); };
    if (Holds(0)) AsAnother(0, false, Room);
    else {
        Begin(0, false);
        Room();
    }
    return Gone + CursorRow - std::min(Rows, CursorRow);
}

void ConsoleModel::Reserved(std::uint32_t Rows)
{
    if (CursorColumn != 0) Newline();
    for (std::uint32_t Row = 0; Row < Rows; Row++) Newline();
}

void ConsoleModel::Fold(std::int32_t X, std::int32_t Y, std::uint32_t Width, std::uint32_t Height,
                        std::span<const ItemCell> Cells, std::uint32_t Page, std::uint32_t ScrollRows,
                        std::uint32_t CaretX, std::uint32_t CaretY, bool Caret)
{
    const auto Laid = [&] { Folded(X, Y, Width, Height, Cells, Page, ScrollRows, CaretX, CaretY, Caret); };
    if (Holds(0)) {
        AsAnother(0, false, Laid);
        return;
    }
    Begin(0, false);
    Laid();
}

void ConsoleModel::Folded(std::int32_t X, std::int32_t Y, std::uint32_t Width, std::uint32_t Height,
                          std::span<const ItemCell> Cells, std::uint32_t Page, std::uint32_t ScrollRows,
                          std::uint32_t CaretX, std::uint32_t CaretY, bool Caret)
{
    for (std::uint32_t Row = 0; Row < ScrollRows; Row++) Scroll();
    if (ScrollRows != 0)
        CursorRow = CursorRow > ScrollRows ? CursorRow - ScrollRows : 0;

    // Folded into the text, a cell keeps its glyph and colours, not its mark.
    for (std::uint32_t Row = 0; Row < Height; Row++) {
        for (std::uint32_t Col = 0; Col < Width; Col++) {
            const std::size_t Index = static_cast<std::size_t>(Y + static_cast<std::int32_t>(Row)) * this->Width() +
                                      static_cast<std::size_t>(X + static_cast<std::int32_t>(Col));
            ItemCell Shown;
            if (!CellOver(Cells[static_cast<std::size_t>(Row) * Width + Col], Grid.At(Index), Page, Shown)) continue;
            Shown.Mark    = ITEM_MARK_NONE;
            Shown.MarkInk = 0;
            Grid.Put(Index, Shown);
        }
    }

    if (Caret) {
        CursorColumn = CaretX;
        CursorRow    = CaretY;
    }
}
