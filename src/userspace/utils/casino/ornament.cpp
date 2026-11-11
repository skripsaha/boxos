/* ornament.cpp — rules, boxes, cloth and hints drawn with the font (ornament.h). */

#include "ornament.h"

#include <algorithm>
#include <vector>

#include "boxos_font.h"
#include "common.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

namespace casino {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;

// The glyphs a cell may be drawn with where two colors meet: blocks, halves,
// the shades, and the thin rules.
constexpr unsigned char Shapes[] = {0x20, 0xB0, 0xB1, 0xB2, 0xDB, 0xDF, 0xDC, 0xDD, 0xDE, 0xC4, 0xB3};

std::int32_t Cells(std::string_view Words)
{
    return static_cast<std::int32_t>(tui::CellsOf(Words));
}

std::int32_t HintWidth(const Hint &One)
{
    return Cells(One.Key) + 1 + Cells(One.What);
}

std::int32_t PutHint(tui::Frame &Out, std::int32_t Col, std::int32_t Row, const Hint &One, const Look &Shown, Color Bar)
{
    Out.Write({Col, Row}, One.Key, tui::TextStyle{Shown.Gold, Bar});
    Out.Write({Col + Cells(One.Key) + 1, Row}, One.What, tui::TextStyle{Shown.Quiet, Bar});
    return HintWidth(One);
}

// The cells over Area darkened, as a shadow falls on them.
void Dim(tui::Frame &Out, painter::core::Rect Area, float Factor)
{
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        for (std::int32_t Col = Area.X; Col < Area.X + Area.Width; Col++) {
            if (Col < 0 || Row < 0 || Col >= Out.Size().Width || Row >= Out.Size().Height) continue;
            tui::Cell Under = Out.Read({Col, Row});
            Under.Foreground = Shade(Under.Foreground, Factor);
            Under.Background = Shade(Under.Background, Factor);
            Out.Put({Col, Row}, Under);
        }
    }
}

}  // namespace

void Settle(tui::Frame &Out, painter::core::Point At, Color In, Color Outside, const std::array<std::uint8_t, 16> &Mask, bool Dither)
{
    const auto Density = [](const std::uint8_t *Rows, std::array<int, 8> &Blocks) {
        Blocks.fill(0);
        for (std::size_t Row = 0; Row < 16; Row++)
            for (int Col = 0; Col < 8; Col++)
                if ((Rows[Row] >> (7 - Col)) & 1) Blocks[(Row / 4) * 2 + static_cast<std::size_t>(Col / 4)]++;
    };
    std::array<int, 8> Wanted;
    Density(Mask.data(), Wanted);
    int Total = 0;
    for (const int Count : Wanted) Total += Count;
    if (Total == 0) return;
    int           Best  = 1 << 20;
    unsigned char Glyph = 0x20;
    bool          Swap  = false;
    for (const unsigned char Shape : Shapes) {
        if (!Dither && ((Shape >= 0xB0 && Shape <= 0xB2) || Shape == 0xDD || Shape == 0xDE || Shape == 0xC4 || Shape == 0xB3)) continue;
        std::array<int, 8> Have;
        Density(BoxFontGlyphs[Shape], Have);
        int Plain = 0, Flipped = 0;
        for (std::size_t Block = 0; Block < 8; Block++) {
            Plain += (Have[Block] - Wanted[Block]) * (Have[Block] - Wanted[Block]);
            Flipped += (16 - Have[Block] - Wanted[Block]) * (16 - Have[Block] - Wanted[Block]);
        }
        if (Plain < Best) {
            Best  = Plain;
            Glyph = Shape;
            Swap  = false;
        }
        if (Flipped < Best) {
            Best  = Flipped;
            Glyph = Shape;
            Swap  = true;
        }
    }
    Out.Put(At, static_cast<char>(Glyph), Swap ? tui::TextStyle{Outside, In} : tui::TextStyle{In, Outside});
}

std::int32_t Lightness(Color Ink)
{
    return (Ink.Red() * 30 + Ink.Green() * 59 + Ink.Blue() * 11) / 100;
}

void HintBar(tui::Frame &Out, const Look &Shown, std::span<const Hint> Hints)
{
    const std::int32_t Row = Out.Size().Height - 1, Wide = Out.Size().Width;
    const Color        Bar = Shade(Shown.Page, 1.12f);
    Out.Fill(painter::core::Rect{0, Row, Wide, 1}, ' ', tui::TextStyle{Shown.Quiet, Bar});
    const Hint Tail[2] = {{"tab", "tables"}, {Spelled(Shown.Keys[KeyHelp]), "keys"}};
    std::int32_t Room = Wide - 2;
    for (const Hint &One : Tail) Room -= HintWidth(One) + 2;
    std::int32_t Col = 1;
    for (const Hint &One : Hints) {
        if (Col - 1 + HintWidth(One) > Room) break;
        Col += PutHint(Out, Col, Row, One, Shown, Bar) + 2;
    }
    std::int32_t From = Wide - 1;
    for (std::size_t At = 2; At-- > 0;) {
        From -= HintWidth(Tail[At]);
        PutHint(Out, From, Row, Tail[At], Shown, Bar);
        From -= 2;
    }
}

void Box(tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title, Color TitleInk)
{
    Dim(Out, painter::core::Rect{Area.X + 1, Area.Y + Area.Height, Area.Width, 1}, 0.35f);
    Dim(Out, painter::core::Rect{Area.X + Area.Width, Area.Y + 1, 1, Area.Height}, 0.35f);
    Panel(Out, Area, Edge, Fill, Title, TitleInk);
}

void Panel(tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title, Color TitleInk)
{
    const tui::TextStyle Line{Edge, Fill};
    Out.Fill(Area, ' ', Line);
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    Rule(Out, {Area.X + 1, Area.Y}, Area.Width - 2, false, Edge);
    Rule(Out, {Area.X + 1, Bottom}, Area.Width - 2, false, Edge);
    Rule(Out, {Area.X, Area.Y + 1}, Area.Height - 2, true, Edge);
    Rule(Out, {Right, Area.Y + 1}, Area.Height - 2, true, Edge);
    Out.Put({Area.X, Area.Y}, Glyphs::CornerTL, Line);
    Out.Put({Right, Area.Y}, Glyphs::CornerTR, Line);
    Out.Put({Area.X, Bottom}, Glyphs::CornerBL, Line);
    Out.Put({Right, Bottom}, Glyphs::CornerBR, Line);
    if (Title.empty()) return;
    Out.Put({Area.X + 2, Area.Y}, Glyphs::TeeRight, Line);
    Out.Write({Area.X + 3, Area.Y}, std::string(" ").append(Title).append(" "), tui::TextStyle{TitleInk, Fill});
    Out.Put({Area.X + 3 + Cells(Title) + 2, Area.Y}, Glyphs::TeeLeft, Line);
}

void BoxRule(tui::Frame &Out, painter::core::Rect Area, std::int32_t Row, Color Edge, Color Fill)
{
    const tui::TextStyle Line{Edge, Fill};
    Rule(Out, {Area.X + 1, Row}, Area.Width - 2, false, Edge);
    Out.Put({Area.X, Row}, Glyphs::TeeLeft, Line);
    Out.Put({Area.X + Area.Width - 1, Row}, Glyphs::TeeRight, Line);
}

void Cloth(tui::Frame &Out, painter::core::Rect Area, Color Felt)
{
    const float HalfAcross = std::max(1.0f, static_cast<float>(Area.Width) / 2.0f);
    const float HalfDown   = std::max(1.0f, static_cast<float>(Area.Height) / 2.0f);
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        const float Down = (static_cast<float>(Row - Area.Y) + 0.5f - HalfDown) / HalfDown;
        for (std::int32_t Col = Area.X; Col < Area.X + Area.Width; Col++) {
            const float Across = (static_cast<float>(Col - Area.X) + 0.5f - HalfAcross) / HalfAcross;
            const float Far    = std::min(Across * Across + Down * Down, 1.4f) / 1.4f;
            const Color Cell   = Shade(Felt, 1.08f - 0.30f * Far);
            Out.Put({Col, Row}, ' ', tui::TextStyle{Cell, Cell});
        }
    }
}

void Weave(tui::Frame &Out, painter::core::Rect Area)
{
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        for (std::int32_t Col = Area.X; Col < Area.X + Area.Width; Col++) {
            const tui::Cell Bare = Out.Read({Col, Row});
            if (Bare.Glyph != ' ' || Bare.Foreground != Bare.Background) continue;
            Out.Put({Col, Row}, Glyphs::LightShade, tui::TextStyle{Shade(Bare.Background, 1.07f), Bare.Background});
        }
    }
}

void Rail(tui::Frame &Out, painter::core::Rect Area, Color Wood, Color Inlay, bool WithInlay)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t Col = Area.X; Col <= Right; Col++) {
        Out.Put({Col, Area.Y}, Glyphs::UpperHalf, tui::TextStyle{Wood, Out.Read({Col, Area.Y}).Background});
        Out.Put({Col, Bottom}, Glyphs::LowerHalf, tui::TextStyle{Wood, Out.Read({Col, Bottom}).Background});
    }
    for (std::int32_t Row = Area.Y + 1; Row < Bottom; Row++) {
        for (const std::int32_t Col : {Area.X, Right}) Out.Put({Col, Row}, Glyphs::RuleDown, tui::TextStyle{Shade(Wood, 1.35f), Wood});
    }
    if (!WithInlay) return;
    const painter::core::Rect In{Area.X + 1, Area.Y + 1, Area.Width - 2, Area.Height - 2};
    const auto Line = [&](std::int32_t Col, std::int32_t Row, char Glyph) {
        Out.Put({Col, Row}, Glyph, tui::TextStyle{Inlay, Out.Read({Col, Row}).Background});
    };
    for (std::int32_t Col = In.X + 1; Col < In.X + In.Width - 1; Col++) {
        Line(Col, In.Y, Glyphs::RuleAcross);
        Line(Col, In.Y + In.Height - 1, Glyphs::RuleAcross);
    }
    for (std::int32_t Row = In.Y + 1; Row < In.Y + In.Height - 1; Row++) {
        Line(In.X, Row, Glyphs::RuleDown);
        Line(In.X + In.Width - 1, Row, Glyphs::RuleDown);
    }
    Line(In.X, In.Y, Glyphs::CornerTL);
    Line(In.X + In.Width - 1, In.Y, Glyphs::CornerTR);
    Line(In.X, In.Y + In.Height - 1, Glyphs::CornerBL);
    Line(In.X + In.Width - 1, In.Y + In.Height - 1, Glyphs::CornerBR);
}

void Print(tui::Frame &Out, painter::core::Point At, std::string_view Words, Color Ink)
{
    for (const char Glyph : Words) {
        Out.Put(At, Glyph, tui::TextStyle{Ink, Out.Read(At).Background});
        At.X++;
    }
}

void Rule(tui::Frame &Out, painter::core::Point At, std::int32_t Length, bool Down, Color Ink)
{
    for (std::int32_t Step = 0; Step < Length; Step++) {
        const painter::core::Point Here{At.X + (Down ? 0 : Step), At.Y + (Down ? Step : 0)};
        Out.Put(Here, Down ? Glyphs::RuleDown : Glyphs::RuleAcross, tui::TextStyle{Ink, Out.Read(Here).Background});
    }
}

}  // namespace casino
