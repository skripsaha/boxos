/* hexer_paint.cpp — the glass. One Painter frame a paint: a title, the lines
 * of the part, a foot. It decides which lines
 * are in view and changes nothing of the file. */

#include <format>
#include <vector>

#include "hexer.h"

namespace {

using PainterColor = painter::core::Color;

constexpr PainterColor TitleInk     = painter::core::Colors::White;
constexpr PainterColor TitlePaper   = painter::core::Colors::Slate;
constexpr PainterColor Paper        = painter::core::Colors::Black;
constexpr PainterColor OffsetInk    = painter::core::Colors::Slate;
constexpr PainterColor ChangedPaper = painter::core::Colors::DarkRed;
constexpr PainterColor CursorPaper  = painter::core::Colors::White;
constexpr PainterColor ShadowPaper  = painter::core::Colors::DarkGray;
constexpr PainterColor HintInk      = painter::core::Colors::DarkGray;
constexpr PainterColor MessageInk   = painter::core::Colors::Amber;
constexpr PainterColor CommandInk   = painter::core::Colors::White;

/* What a byte is, said by its colour: zero, text, a blank, a control, high. */
PainterColor InkOf(std::uint8_t Byte)
{
    if (Byte == 0) return painter::core::Colors::DarkGray;
    if (Byte == ' ' || Byte == '\t' || Byte == '\n' || Byte == '\r') return painter::core::Colors::Teal;
    if (Byte < 0x20 || Byte == 0x7f) return painter::core::Colors::Amber;
    if (Byte >= 0x80) return painter::core::Colors::Violet;
    return painter::core::Colors::Leaf;
}

constexpr char HexDigit[] = "0123456789abcdef";

}

std::uint32_t Hexer::HexColumn(std::uint32_t Byte) const
{
    return Digits + 2 + 3 * Byte + (Byte >= Width / 2 ? 1 : 0);
}

std::uint32_t Hexer::TextColumn(std::uint32_t Byte) const
{
    return Digits + 3 * Width + 4 + Byte;
}

void Hexer::Put(std::uint32_t Row, std::uint32_t Col, char Ch, painter::core::Color Fg,
                painter::core::Color Bg)
{
    if (Row >= Rows || Col >= Cols) return;
    Screen->FrameBuffer().Put({static_cast<std::int32_t>(Col), static_cast<std::int32_t>(Row)},
                              painter::tui::Cell{Ch, Fg, Bg});
}

/* A name off the volume is bytes, not letters: what the font has no glyph for
 * is drawn '?'. */
std::uint32_t Hexer::Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text,
                           painter::core::Color Fg, painter::core::Color Bg)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        auto Byte = static_cast<unsigned char>(Ch);
        Put(Row, Col++, Byte >= 0x20 && Byte < 0x7f ? Ch : '?', Fg, Bg);
    }
    return Col;
}

void Hexer::FollowCursor()
{
    std::uint64_t Line = At - At % Width;
    std::uint64_t Span = static_cast<std::uint64_t>(TextRows()) * Width;
    if (Line < Top) Top = Line;
    else if (Line >= Top + Span) Top = Line - Span + Width;
}

void Hexer::PaintTitle()
{
    for (std::uint32_t Col = 0; Col < Cols; Col++) Put(0, Col, ' ', TitleInk, TitlePaper);

    std::uint32_t Col = Write(0, 1, "hexer", TitleInk, TitlePaper);
    Col = Write(0, Col + 2, Name, TitleInk, TitlePaper);
    if (!Tags.empty()) Col = Write(0, Col + 2, std::format("[{}]", Tags), TitleInk, TitlePaper);

    /* Where the cursor is, after the name: the right of the top row is where
     * the clock draws itself. */
    std::string Where = std::format("at 0x{:x}", At);
    if (Bytes.ChangeCount() != 0) Where += std::format("  {} changed", Bytes.ChangeCount());
    Write(0, Col + 3, Where, TitleInk, TitlePaper);
}

void Hexer::PaintLines()
{
    std::uint32_t TextRowCount = TextRows();
    std::size_t   Want = static_cast<std::size_t>(TextRowCount) * Width;
    if (Want > Part.Hi - Top) Want = static_cast<std::size_t>(Part.Hi - Top);

    std::vector<std::uint8_t> Buffer(Want), Changed(Want);
    std::size_t Got = Bytes.Read(Top, Buffer.data(), Want, Changed.data());
    if (Got < Want && Message.empty())
        Message = std::format("the file gave no bytes past 0x{:x}", Top + Got);

    for (std::uint32_t Row = 1; Row <= TextRowCount; Row++) {
        for (std::uint32_t Col = 0; Col < Cols; Col++) Put(Row, Col, ' ', painter::core::Colors::LightGray, Paper);

        std::uint64_t LineAt = Top + static_cast<std::uint64_t>(Row - 1) * Width;
        if (LineAt >= Part.Hi) continue;
        Write(Row, 0, std::format("{:0{}x}", LineAt, Digits), OffsetInk, Paper);

        for (std::uint32_t Byte = 0; Byte < Width; Byte++) {
            std::uint64_t Place = LineAt + Byte;
            if (Place < Part.Lo || Place >= Part.Hi || Place - Top >= Got) continue;

            std::uint8_t Value   = Buffer[Place - Top];
            PainterColor Ink     = InkOf(Value);
            PainterColor HexBg   = Changed[Place - Top] ? ChangedPaper : Paper;
            PainterColor TextBg  = HexBg;
            PainterColor HexFg   = Ink;
            PainterColor TextFg  = Ink;
            if (Place == At) {
                HexBg  = InText ? ShadowPaper : CursorPaper;
                TextBg = InText ? CursorPaper : ShadowPaper;
                HexFg  = InText ? painter::core::Colors::White : painter::core::Colors::Black;
                TextFg = InText ? painter::core::Colors::Black : painter::core::Colors::White;
            }
            Put(Row, HexColumn(Byte), HexDigit[Value >> 4], HexFg, HexBg);
            Put(Row, HexColumn(Byte) + 1, HexDigit[Value & 0xf], HexFg, HexBg);
            Put(Row, TextColumn(Byte), Value >= 0x20 && Value < 0x7f ? static_cast<char>(Value) : '.', TextFg, TextBg);
        }
    }
}

void Hexer::PaintFoot()
{
    std::uint32_t Row = Rows - 1;
    for (std::uint32_t Col = 0; Col < Cols; Col++) Put(Row, Col, ' ', painter::core::Colors::LightGray, Paper);

    if (CommandOpen) {
        Put(Row, 0, ':', CommandInk, Paper);
        Write(Row, 2, Command, CommandInk, Paper);
    } else if (!Message.empty()) {
        Write(Row, 1, Message, MessageInk, Paper);
    } else {
        Write(Row, 1, InText ? "^S save  ^Q quit  TAB to hex   ESC go find save quit help"
                             : "^S save  ^Q quit  TAB to text  ESC go find save quit help",
              HintInk, Paper);
    }
}

bool Hexer::Paint()
{
    FollowCursor();
    PaintTitle();
    PaintLines();
    PaintFoot();

    /* The caret first and the picture last: a caret that
     * arrives before its frame is not seen, one that arrives after is. */
    std::uint32_t CaretRow, CaretCol;
    if (CommandOpen) {
        CaretRow = Rows - 1;
        CaretCol = 2 + static_cast<std::uint32_t>(Command.size());
        if (CaretCol >= Cols) CaretCol = Cols - 1;
    } else {
        std::uint32_t Byte = static_cast<std::uint32_t>(At % Width);
        CaretRow = 1 + static_cast<std::uint32_t>((At - At % Width - Top) / Width);
        CaretCol = InText ? TextColumn(Byte) : HexColumn(Byte) + (LowHalf ? 1 : 0);
    }
    if (!Screen->SetCursor({static_cast<std::int32_t>(CaretCol), static_cast<std::int32_t>(CaretRow)})) return false;
    return static_cast<bool>(Screen->Present());
}
