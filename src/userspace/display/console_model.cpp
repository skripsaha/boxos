#include "console_model.h"

#include <algorithm>
#include <cstring>

#include "box/color.h"
#include "boxos_glyph_fold.h"

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
    Clear(COLOR_LIGHT_GRAY, COLOR_BLACK);
}

void ConsoleModel::Clear(std::uint32_t Foreground, std::uint32_t Background)
{
    const TextCell Blank{Foreground, Background, ' ', {0, 0, 0}};
    std::fill(Surface.begin(), Surface.end(), Blank);
    CursorColumn = 0;
    CursorRow = 0;
}

void ConsoleModel::Scroll()
{
    if (HeightValue == 0 || WidthValue == 0) return;
    const std::size_t RowBytes = static_cast<std::size_t>(WidthValue) * sizeof(TextCell);
    std::memmove(Surface.data(), Surface.data() + WidthValue,
                 static_cast<std::size_t>(HeightValue - 1u) * RowBytes);
    const TextCell Blank{COLOR_LIGHT_GRAY, COLOR_BLACK, ' ', {0, 0, 0}};
    std::fill(Surface.end() - WidthValue, Surface.end(), Blank);
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
}

void ConsoleModel::Write(const char *Bytes, std::size_t Length,
                         std::uint32_t Foreground, std::uint32_t Background)
{
    std::size_t Offset = 0;
    while (Offset < Length) {
        const char Byte = Bytes[Offset];
        if (static_cast<std::uint8_t>(Byte) < 0x80u) {
            switch (Byte) {
            case '\n': Newline(); break;
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

void ConsoleModel::Apply(const ConsoleRun &Run)
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
        Write(Run.text, std::min<std::size_t>(Run.len, CONSOLE_RUN_TEXT_MAX), Run.fg, Run.bg);
        return;
    default:
        return;
    }
}

bool ConsoleModel::CommitLayer(std::int32_t X, std::int32_t Y, std::uint32_t Width,
                               std::uint32_t Height,
                               std::span<const DisplayLayerCell> Cells,
                               std::uint32_t ScrollRows,
                               std::uint32_t CursorX, std::uint32_t CursorY,
                               bool CursorVisible)
{
    if (Width == 0 || Height == 0 || X < 0 || Y < 0 ||
        static_cast<std::uint32_t>(X) + Width > WidthValue ||
        static_cast<std::uint32_t>(Y) + Height > HeightValue)
        return false;

    const std::size_t Count = static_cast<std::size_t>(Width) * Height;
    if (Cells.size() != Count || ScrollRows > HeightValue) return false;
    if (CursorVisible &&
        (CursorX >= Width || CursorY >= Height ||
         static_cast<std::uint32_t>(X) + CursorX >= WidthValue ||
         static_cast<std::uint32_t>(Y) + CursorY >= HeightValue))
        return false;

    for (std::uint32_t Row = 0; Row < ScrollRows; Row++) Scroll();
    if (ScrollRows != 0)
        CursorRow = CursorRow > ScrollRows ? CursorRow - ScrollRows : 0;

    for (std::uint32_t Row = 0; Row < Height; Row++) {
        for (std::uint32_t Col = 0; Col < Width; Col++) {
            const DisplayLayerCell &Cell = Cells[static_cast<std::size_t>(Row) * Width + Col];
            if (!Cell.Opaque) continue;
            Surface[static_cast<std::size_t>(Y + static_cast<std::int32_t>(Row)) * WidthValue +
                    X + static_cast<std::int32_t>(Col)] = Cell.Cell;
        }
    }

    if (CursorVisible) {
        CursorColumn = static_cast<std::uint32_t>(X) + CursorX;
        CursorRow = static_cast<std::uint32_t>(Y) + CursorY;
    }
    return true;
}
