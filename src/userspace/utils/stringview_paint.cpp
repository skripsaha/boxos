/* stringview_paint.cpp — the glass. One frame of cells a paint, laid down by
 * one vga_paint: a title, the texts the filter keeps — where each begins,
 * u16 for the wide ones, the text with what was typed marked in it — and a
 * foot with what is typed. */

#include <format>

#include "box/color.h"
#include "box/vga.h"

#include "stringview.h"

namespace {

constexpr Color TitleInk   = COLOR_WHITE;
constexpr Color TitlePaper = COLOR_SLATE;
constexpr Color Paper      = COLOR_BLACK;
constexpr Color TextInk    = COLOR_LIGHT_GRAY;
constexpr Color OffsetInk  = COLOR_SLATE;
constexpr Color WideInk    = COLOR_AMBER;
constexpr Color HintInk    = COLOR_DARK_GRAY;
constexpr Color TroubleInk = COLOR_AMBER;
constexpr Color FindInk    = COLOR_WHITE;
constexpr Color MarkInk    = COLOR_BLACK;
constexpr Color MarkPaper  = COLOR_TEAL;

/* How many bytes the UTF-8 character led by Lead takes. */
std::size_t CharacterBytes(unsigned char Lead)
{
    if (Lead < 0xc0) return 1;
    if (Lead < 0xe0) return 2;
    if (Lead < 0xf0) return 3;
    return 4;
}

}

void Stringview::Put(std::uint32_t Row, std::uint32_t Col, char Ch, Color Fg, Color Bg)
{
    if (Row >= Rows || Col >= Cols) return;
    TextCell &Cell = Frame[Row * Cols + Col];
    Cell = TextCell{};
    Cell.fg = Fg;
    Cell.bg = Bg;
    Cell.ch = Ch;
}

std::uint32_t Stringview::Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, Color Fg, Color Bg)
{
    for (char Ch : Text) {
        if (Col >= Cols) break;
        auto Byte = static_cast<unsigned char>(Ch);
        Put(Row, Col++, Byte >= 0x20 && Byte < 0x7f ? Ch : '?', Fg, Bg);
    }
    return Col;
}

void Stringview::PaintTitle()
{
    for (std::uint32_t Col = 0; Col < Cols; Col++) Put(0, Col, ' ', TitleInk, TitlePaper);

    std::uint32_t Col = Write(0, 1, "stringview", TitleInk, TitlePaper);
    Col = Write(0, Col + 2, Name, TitleInk, TitlePaper);
    if (!Tags.empty()) Col = Write(0, Col + 2, std::format("[{}]", Tags), TitleInk, TitlePaper);

    /* The count after the name: the right of the top row is where the clock
     * draws itself. */
    std::string Count = Filter.empty() ? std::format("{} text{}", Texts.size(), Texts.size() == 1 ? "" : "s")
                                       : std::format("{} of {} texts", Shown.size(), Texts.size());
    if (Sifting) Count = std::format("sifting {}%  {}", SiftedTo * 100 / Size, Count);
    Write(0, Col + 3, Count, TitleInk, TitlePaper);
}

void Stringview::PaintList()
{
    std::uint32_t Base = Digits + 7;

    for (std::uint32_t Row = 1; Row <= ListRows(); Row++) {
        for (std::uint32_t Col = 0; Col < Cols; Col++) Put(Row, Col, ' ', TextInk, Paper);

        std::size_t Index = Top + Row - 1;
        if (Index >= Shown.size()) {
            if (Row == 1 && !Sifting)
                Write(1, 1,
                      Texts.empty() ? std::format("no text of {} characters or more", Shortest)
                                    : std::format("no text holds \"{}\"", Filter),
                      HintInk, Paper);
            continue;
        }

        const StringviewText &Found = Texts[Shown[Index]];
        Write(Row, 0, std::format("{:0{}x}", Found.Offset, Digits), OffsetInk, Paper);
        if (Found.Wide) Write(Row, Digits + 2, "u16", WideInk, Paper);

        /* One cell a character; a character the font cannot draw is '?'. */
        std::size_t   Mark = StringviewFind(Found.Text, Filter);
        std::size_t   MarkEnd = Filter.empty() ? 0 : Mark + Filter.size();
        std::uint32_t Character = 0;
        for (std::size_t Byte = 0; Byte < Found.Text.size(); Character++) {
            auto        Lead  = static_cast<unsigned char>(Found.Text[Byte]);
            std::size_t Taken = CharacterBytes(Lead);
            if (Character >= Left) {
                std::uint32_t Col = Base + Character - Left;
                if (Col + 1 >= Cols && Byte + Taken < Found.Text.size()) {
                    Put(Row, Cols - 1, '>', HintInk, Paper);
                    break;
                }
                bool Marked = Byte >= Mark && Byte < MarkEnd;
                bool Drawn  = Lead >= 0x20 && Lead < 0x7f;
                Put(Row, Col, Lead == '\t' ? ' ' : Drawn ? static_cast<char>(Lead) : '?',
                    Marked ? MarkInk : Drawn ? TextInk : HintInk, Marked ? MarkPaper : Paper);
            }
            Byte += Taken;
        }
        if (Left > 0 && Character > 0) Put(Row, Base - 1, '<', HintInk, Paper);
    }
}

void Stringview::PaintFoot()
{
    std::uint32_t Row = Rows - 1;
    for (std::uint32_t Col = 0; Col < Cols; Col++) Put(Row, Col, ' ', TextInk, Paper);

    std::uint32_t Col = Write(Row, 0, "find ", HintInk, Paper);
    Col = Write(Row, Col, Filter, FindInk, Paper);

    std::string_view Right = !Trouble.empty() ? std::string_view(Trouble)
                             : Filter.empty() ? std::string_view("type to narrow   ESC or ^Q leave")
                                              : std::string_view("Backspace  ESC clear   ^Q leave");
    Color Ink = Trouble.empty() ? HintInk : TroubleInk;
    if (Col + 2 + Right.size() + 1 <= Cols)
        Write(Row, Cols - static_cast<std::uint32_t>(Right.size()) - 1, Right, Ink, Paper);
}

void Stringview::Paint()
{
    PaintTitle();
    PaintList();
    PaintFoot();

    std::uint32_t CaretCol = 5 + static_cast<std::uint32_t>(Filter.size());
    if (CaretCol >= Cols) CaretCol = Cols - 1;
    (void)vga_setcursor(static_cast<std::uint8_t>(Rows - 1), static_cast<std::uint8_t>(CaretCol));
    (void)vga_paint(0, 0, static_cast<std::uint8_t>(Rows), static_cast<std::uint8_t>(Cols), Frame.data());
}
