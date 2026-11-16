/* stringview_paint.cpp — the glass. One Painter frame a paint: a title, the
 * texts the filter keeps — where each begins,
 * u16 for the wide ones, the text with what was typed marked in it — and a
 * foot with what is typed. */

#include <format>

#include "painter/tui/text.h"

#include "stringview.h"

namespace {

using PainterColor = painter::core::Color;

constexpr PainterColor TitleInk   = painter::core::Colors::White;
constexpr PainterColor TitlePaper = painter::core::Colors::Slate;
constexpr PainterColor Paper      = painter::core::Colors::Black;
constexpr PainterColor TextInk    = painter::core::Colors::LightGray;
constexpr PainterColor OffsetInk  = painter::core::Colors::Slate;
constexpr PainterColor WideInk    = painter::core::Colors::Amber;
constexpr PainterColor HintInk    = painter::core::Colors::DarkGray;
constexpr PainterColor TroubleInk = painter::core::Colors::Amber;
constexpr PainterColor FindInk    = painter::core::Colors::White;
constexpr PainterColor MarkInk    = painter::core::Colors::Black;
constexpr PainterColor MarkPaper  = painter::core::Colors::Teal;

/* How many bytes the UTF-8 character led by Lead takes. */
std::size_t CharacterBytes(unsigned char Lead)
{
    if (Lead < 0xc0) return 1;
    if (Lead < 0xe0) return 2;
    if (Lead < 0xf0) return 3;
    return 4;
}

}

void Stringview::Put(std::uint32_t Row, std::uint32_t Col, char32_t Ch, painter::core::Color Fg,
                     painter::core::Color Bg)
{
    if (Row >= Rows || Col >= Cols) return;
    Screen->FrameBuffer().Put({static_cast<std::int32_t>(Col), static_cast<std::int32_t>(Row)},
                              painter::tui::Cell{Ch, Fg, Bg});
}

std::uint32_t Stringview::Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text,
                                painter::core::Color Fg, painter::core::Color Bg)
{
    for (std::size_t Byte = 0; Byte < Text.size() && Col < Cols;) {
        const painter::tui::Glyph Shape = painter::tui::GlyphAt(Text, Byte);
        Put(Row, Col++, Shape.Odd || Shape.Character == U'\t' ? U'?' : Shape.Character, Fg, Bg);
        Byte += Shape.Length;
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

        /* One cell a character; what is no character to show is '?'. */
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
                const painter::tui::Glyph Shape = painter::tui::GlyphAt(Found.Text, Byte);
                bool Marked = Byte >= Mark && Byte < MarkEnd;
                bool Drawn  = !Shape.Odd && Lead != '\t';
                Put(Row, Col, Lead == '\t' ? U' ' : Drawn ? Shape.Character : U'?',
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
    PainterColor Ink = Trouble.empty() ? HintInk : TroubleInk;
    if (Col + 2 + Right.size() + 1 <= Cols)
        Write(Row, Cols - static_cast<std::uint32_t>(Right.size()) - 1, Right, Ink, Paper);
}

bool Stringview::Paint()
{
    PaintTitle();
    PaintList();
    PaintFoot();

    std::uint32_t CaretCol = 5 + static_cast<std::uint32_t>(Filter.size());
    if (CaretCol >= Cols) CaretCol = Cols - 1;
    (void)Screen->FrameBuffer().SetCaret(
        painter::core::Point{static_cast<std::int32_t>(CaretCol), static_cast<std::int32_t>(Rows - 1)});
    return static_cast<bool>(Screen->Present());
}
