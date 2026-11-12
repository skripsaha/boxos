#include "console_model.h"

#include <algorithm>
#include <cstring>

#include "box/color.h"
#include "boxos_glyph_fold.h"
#include "boxos_log_tone.h"

namespace {

std::uint8_t Utf8Promise(std::uint8_t Byte) noexcept
{
    if (Byte >= 0xC2u && Byte <= 0xDFu) return 2;
    if (Byte >= 0xE0u && Byte <= 0xEFu) return 3;
    if (Byte >= 0xF0u && Byte <= 0xF4u) return 4;
    return 0;
}

int DecodeUtf8(const char *Bytes, std::size_t Length, std::uint32_t &Codepoint) noexcept
{
    if (Length == 0) return 0;
    const std::uint8_t Lead = static_cast<std::uint8_t>(Bytes[0]);
    const std::uint8_t Need = Utf8Promise(Lead);
    if (Need == 0 || Length < Need) return 0;

    std::uint32_t Value = 0;
    if (Need == 2) Value = Lead & 0x1Fu;
    if (Need == 3) Value = Lead & 0x0Fu;
    if (Need == 4) Value = Lead & 0x07u;
    for (std::uint8_t Index = 1; Index < Need; Index++) {
        const std::uint8_t Byte = static_cast<std::uint8_t>(Bytes[Index]);
        if ((Byte & 0xC0u) != 0x80u) return 0;
        Value = (Value << 6) | (Byte & 0x3Fu);
    }
    if ((Need == 2 && Value < 0x80u) || (Need == 3 && Value < 0x800u) ||
        (Need == 4 && (Value < 0x10000u || Value > 0x10FFFFu)) ||
        (Value >= 0xD800u && Value <= 0xDFFFu))
        return 0;
    Codepoint = Value;
    return Need;
}

}  // namespace

ConsoleModel::ConsoleModel(std::uint32_t Width, std::uint32_t Height)
    : WidthValue(Width), HeightValue(Height),
      Surface(static_cast<std::size_t>(Width) * Height)
{
    // A line is never longer than the console: lifting one never allocates,
    // so no frame drawn while a line is typed can fail for want of memory.
    Lifted.reserve(Surface.size());
    Clear(COLOR_DEFAULT, COLOR_DEFAULT);
}

void ConsoleModel::Clear(std::uint32_t Foreground, std::uint32_t Background)
{
    const TextCell Blank{Foreground, Background, ' ', {0, 0, 0}};
    std::fill(Surface.begin(), Surface.end(), Blank);
    CursorColumn = 0;
    CursorRow = 0;
    LineStart = LineEnd = Others = 0;
    Held = false;
}

void ConsoleModel::Scroll()
{
    if (HeightValue == 0 || WidthValue == 0) return;
    const std::size_t RowBytes = static_cast<std::size_t>(WidthValue) * sizeof(TextCell);
    std::memmove(Surface.data(), Surface.data() + WidthValue,
                 static_cast<std::size_t>(HeightValue - 1u) * RowBytes);
    const TextCell Blank{COLOR_DEFAULT, COLOR_DEFAULT, ' ', {0, 0, 0}};
    std::fill(Surface.end() - WidthValue, Surface.end(), Blank);
    // What is kept as places on the console moves up with it.
    for (std::size_t *Place : {&LineStart, &LineEnd, &Others})
        *Place = *Place >= WidthValue ? *Place - WidthValue : 0;
}

void ConsoleModel::Newline()
{
    CursorColumn = 0;
    CursorRow++;
    if (CursorRow >= HeightValue) {
        Scroll();
        CursorRow = HeightValue == 0 ? 0 : HeightValue - 1u;
    }
}

void ConsoleModel::CarriageReturn()
{
    CursorColumn = 0;
}

void ConsoleModel::Backspace(std::uint32_t Foreground, std::uint32_t Background)
{
    if (CursorColumn == 0 || CursorRow >= HeightValue) return;
    CursorColumn--;
    Surface[static_cast<std::size_t>(CursorRow) * WidthValue + CursorColumn] =
        TextCell{Foreground, Background, ' ', {0, 0, 0}};
}

void ConsoleModel::Tab()
{
    const std::uint32_t Target = (CursorColumn + 8u) & ~7u;
    if (Target >= WidthValue) Newline();
    else CursorColumn = Target;
}

void ConsoleModel::WriteCell(char Glyph, std::uint32_t Foreground, std::uint32_t Background)
{
    if (CursorRow < HeightValue && CursorColumn < WidthValue) {
        Surface[static_cast<std::size_t>(CursorRow) * WidthValue + CursorColumn] =
            TextCell{Foreground, Background, Glyph, {0, 0, 0}};
    }
    CursorColumn++;
    if (CursorColumn >= WidthValue) Newline();
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
            default:   WriteCell(Byte, Foreground, Background); break;
            }
            Offset++;
            continue;
        }

        std::uint32_t Codepoint = 0;
        const int Decoded = DecodeUtf8(Bytes + Offset, Length - Offset, Codepoint);
        if (Decoded == 0) {
            WriteCell(Byte, Foreground, Background);
            Offset++;
            continue;
        }
        char Cells[BOXOS_GLYPH_FOLD_MAX]{};
        const int Count = BoxGlyphFoldCodepoint(Codepoint, Cells);
        for (int Index = 0; Index < Count; Index++)
            WriteCell(Cells[Index], Foreground, Background);
        Offset += static_cast<std::size_t>(Decoded);
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
    if (WidthValue == 0 || HeightValue == 0) return;
    std::int64_t Linear = static_cast<std::int64_t>(CursorRow) * WidthValue + CursorColumn;
    Linear += Delta;
    const std::int64_t Last = static_cast<std::int64_t>(WidthValue) * HeightValue - 1;
    Linear = std::clamp<std::int64_t>(Linear, 0, Last);
    CursorRow = static_cast<std::uint32_t>(Linear / WidthValue);
    CursorColumn = static_cast<std::uint32_t>(Linear % WidthValue);
}

std::size_t ConsoleModel::Here() const noexcept
{
    return static_cast<std::size_t>(CursorRow) * WidthValue + CursorColumn;
}

void ConsoleModel::MoveTo(std::size_t Linear) noexcept
{
    if (WidthValue == 0 || HeightValue == 0) return;
    Linear       = std::min(Linear, Surface.size() - 1u);
    CursorRow    = static_cast<std::uint32_t>(Linear / WidthValue);
    CursorColumn = static_cast<std::uint32_t>(Linear % WidthValue);
}

// One cell laid as it was, the caret after it — at the end of a row on the
// next, the console scrolled when that is past its last.
void ConsoleModel::Lay(const TextCell &Cell)
{
    if (CursorRow < HeightValue && CursorColumn < WidthValue)
        Surface[static_cast<std::size_t>(CursorRow) * WidthValue + CursorColumn] = Cell;
    CursorColumn++;
    if (CursorColumn >= WidthValue) Newline();
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
    const std::size_t End = std::min(LineEnd, Surface.size());
    Lifted.assign(Surface.begin() + static_cast<std::ptrdiff_t>(LineStart),
                  Surface.begin() + static_cast<std::ptrdiff_t>(End));
    const std::size_t Caret = std::min(Here() >= LineStart ? Here() - LineStart : 0, Lifted.size());
    const TextCell    Blank{COLOR_DEFAULT, COLOR_DEFAULT, ' ', {0, 0, 0}};
    std::fill(Surface.begin() + static_cast<std::ptrdiff_t>(LineStart),
              Surface.begin() + static_cast<std::ptrdiff_t>(End), Blank);

    MoveTo(Held ? Others : LineStart);
    if (Held && Text && Lane != OthersLane && CursorColumn != 0) Newline();
    Said();
    Others     = Here();
    OthersLane = Lane;
    Held       = true;

    if (CursorColumn != 0) Newline();
    LineStart = Here();
    for (const TextCell &Cell : Lifted) Lay(Cell);
    LineEnd  = Here();
    LastLane = TypingLane;
    MoveTo(LineStart + Caret);
}

void ConsoleModel::Away(std::uint32_t Lane)
{
    Forget(Lane);
    if (TypingLane == Lane) TypingLane = 0;   // its line stays where it is, held under nothing
    if (Lane == 0 || LastLane != Lane || LineEnd <= LineStart) return;
    const std::size_t End = std::min(LineEnd, Surface.size());
    Kept              One;
    One.Lane = Lane;
    One.Line.assign(Surface.begin() + static_cast<std::ptrdiff_t>(LineStart),
                    Surface.begin() + static_cast<std::ptrdiff_t>(End));
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
    for (const TextCell &Cell : One.Line) Lay(Cell);
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
        Clear(Run.fg, Run.bg);
        return;
    case CONSOLE_RUN_STEP: {
        std::int32_t Delta = 0;
        if (Run.len >= sizeof(Delta)) std::memcpy(&Delta, Run.text, sizeof(Delta));
        Step(Delta);
        return;
    }
    case CONSOLE_RUN_TEXT:
    case CONSOLE_RUN_HINT:
        Write(Run.text, std::min<std::size_t>(Run.len, CONSOLE_RUN_TEXT_MAX), Run.fg, Run.bg);
        return;
    default:
        return;
    }
}

bool ShownOver(const TextCell &Layered, const TextCell &Beneath, std::uint32_t Page, TextCell &Shown) noexcept
{
    const bool FrontThrough = Layered.fg == COLOR_THROUGH;
    const bool BackThrough = Layered.bg == COLOR_THROUGH;
    if (FrontThrough && BackThrough) {
        Shown = Beneath;
        return false;
    }
    const std::uint32_t Seen = (Beneath.bg >> 24) ? Page : Beneath.bg;
    Shown = Layered;
    if (FrontThrough) Shown.fg = Seen;
    if (BackThrough) Shown.bg = Seen;
    return true;
}

void ConsoleModel::CommitLayer(std::int32_t X, std::int32_t Y, std::uint32_t Width,
                               std::uint32_t Height,
                               std::span<const DisplayLayerCell> Cells,
                               std::uint32_t Page, std::uint32_t ScrollRows,
                               std::uint32_t CursorX, std::uint32_t CursorY,
                               bool CursorVisible)
{
    const auto Fold = [&] { Folded(X, Y, Width, Height, Cells, Page, ScrollRows, CursorX, CursorY, CursorVisible); };
    if (Holds(0)) {
        AsAnother(0, false, Fold);
        return;
    }
    Begin(0, false);
    Fold();
}

void ConsoleModel::Folded(std::int32_t X, std::int32_t Y, std::uint32_t Width, std::uint32_t Height,
                          std::span<const DisplayLayerCell> Cells, std::uint32_t Page, std::uint32_t ScrollRows,
                          std::uint32_t CursorX, std::uint32_t CursorY, bool CursorVisible)
{
    for (std::uint32_t Row = 0; Row < ScrollRows; Row++) Scroll();
    if (ScrollRows != 0)
        CursorRow = CursorRow > ScrollRows ? CursorRow - ScrollRows : 0;

    for (std::uint32_t Row = 0; Row < Height; Row++) {
        for (std::uint32_t Col = 0; Col < Width; Col++) {
            const DisplayLayerCell &Cell = Cells[static_cast<std::size_t>(Row) * Width + Col];
            if (!Cell.Opaque) continue;
            TextCell &Beneath = Surface[static_cast<std::size_t>(Y + static_cast<std::int32_t>(Row)) * WidthValue +
                                        X + static_cast<std::int32_t>(Col)];
            TextCell Shown;
            if (ShownOver(Cell.Cell, Beneath, Page, Shown)) Beneath = Shown;
        }
    }

    if (CursorVisible) {
        CursorColumn = static_cast<std::uint32_t>(X) + CursorX;
        CursorRow = static_cast<std::uint32_t>(Y) + CursorY;
    }
}
