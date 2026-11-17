/*
 * draw stand — text on a canvas (Canvas::FillText, TextPath): glyphs kept
 * and laid as filling the text's shape lays them, to a step of coverage
 * where the pen stands on a pixel's edge and to its ink where it stands
 * between; a line's glyphs that overlap laid once; each ink drawn alone;
 * turned, larger than kept and twice the size, the text the same ink; cut
 * to its box; lines under it; a character no font has drawn as a box; the
 * text stroked and casting a shadow as a shape does; two strands drawing at
 * once alike. In the tree's own fonts, JetBrains Mono, whose advances are
 * whole pixels at 20 to the em.
 */

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

#include "draw_stand.h"

#include "boxos_pixel.h"
#include "painter/core/canvas.h"
#include "painter/core/text.h"
#include "typecase/layout.h"

namespace stand {

using namespace painter::core;
using painter::core::Color;

namespace {

std::vector<std::uint8_t> Load(const char *Path)
{
    std::ifstream In(Path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
}

typecase::Fonts TreeBook()
{
    typecase::Fonts Book;
    for (const char *Path : {"src/userspace/typecase/fonts/JetBrainsMonoNL-Regular.ttf",
                             "src/userspace/typecase/fonts/JetBrainsMonoNL-Bold.ttf"})
        if (auto Face = typecase::Font::Read(Load(Path))) Book.Add(std::move(*Face));
    return Book;
}

typecase::Layout Lay(const typecase::Fonts &Book, const char *Utf8, float Size, typecase::TextBox Box = {},
                     std::vector<typecase::Span> Spans = {}, typecase::Style Style = {})
{
    typecase::Text Text = typecase::Text::Of(Utf8, Style);
    Text.Style.Size     = Size;
    Text.Spans          = std::move(Spans);
    return typecase::Layout::Of(Text, Box, Book);
}

/* The opacity of every pixel, 0 to 1. */
std::vector<double> Opacities(const Canvas &C)
{
    std::vector<double> All;
    for (std::uint32_t Y = 0; Y < C.Height(); Y++)
        for (std::uint32_t X = 0; X < C.Width(); X++) All.push_back(C.Row(Y)[X].Alpha / 65535.0);
    return All;
}

double Sum(const std::vector<double> &Of)
{
    double Total = 0;
    for (double Each : Of) Total += Each;
    return Total;
}

/* Where the ink lies across, its middle. */
double Middle(const Canvas &C)
{
    double Weighted = 0, Total = 0;
    for (std::uint32_t Y = 0; Y < C.Height(); Y++)
        for (std::uint32_t X = 0; X < C.Width(); X++) {
            const double A = C.Row(Y)[X].Alpha / 65535.0;
            Weighted += (X + 0.5) * A, Total += A;
        }
    return Total > 0 ? Weighted / Total : 0;
}

std::string Said(const char *Format, double A, double B = 0, double C = 0)
{
    char Line[300];
    std::snprintf(Line, sizeof(Line), Format, A, B, C);
    return Line;
}

}  // namespace

void CheckText(Tally &Count)
{
    const typecase::Fonts Book = TreeBook();
    const Fill            White(Color::Rgb(255, 255, 255));
    const auto            Make = [] { return *Canvas::Make(360, 120); };

    /* On a pixel's edge: the glyphs kept are the shape filled. */
    {
        const auto Text = Lay(Book, "Hello, \xD0\xBC\xD0\xB8\xD1\x80! {}", 20);
        Canvas     Kept = Make(), Shape = Make();
        Kept.FillText(Text, {10, 10}, White);
        Shape.Fill(TextPath(Text, {10, 10}), White);
        const auto   A = Opacities(Kept), B = Opacities(Shape);
        double       Worst = 0, Mean = 0;
        for (std::size_t I = 0; I < A.size(); I++) Worst = std::max(Worst, std::fabs(A[I] - B[I])), Mean += std::fabs(A[I] - B[I]);
        Mean /= static_cast<double>(A.size());
        Count.Check(Sum(A) > 400 && Worst <= 2.0 / 255 && Mean < 1e-4,
                    Said("text whose pens stand on pixels' edges: its kept glyphs laid as its shape is filled — the "
                         "most two pixels differ %.4f, %.1f pixels of ink",
                         Worst, Sum(A)));
    }
    /* Between: a quarter pixel's place, the same ink. */
    {
        const auto Text = Lay(Book, "Quarters of a pixel", 20);
        Canvas     Kept = Make(), Shape = Make();
        Kept.FillText(Text, {10.3f, 10}, White);
        Shape.Fill(TextPath(Text, {10.3f, 10}), White);
        const double A = Sum(Opacities(Kept)), B = Sum(Opacities(Shape));
        Count.Check(Near(A, B, 0.003) && std::fabs(Middle(Kept) - Middle(Shape)) < 1.0 / 8,
                    Said("text whose pens stand between pixels: its glyphs drawn at the nearest quarter, the ink %.1f "
                         "of the shape's %.1f, its middle %.3f pixels apart",
                         A, B, std::fabs(Middle(Kept) - Middle(Shape))));
    }
    /* Each ink alone. */
    {
        typecase::Style Blue;
        Blue.Ink        = 1;
        const auto Text = Lay(Book, "black blue black", 20, {}, {{6, 10, Blue}});
        Canvas     Own = Make(), Other = Make();
        Own.FillText(Text, {0, 0}, White, {}, 1.0f, 0);
        Other.FillText(Text, {0, 0}, White, {}, 1.0f, 1);
        bool Apart = true;
        for (std::uint32_t Y = 0; Y < 120; Y++)
            for (std::uint32_t X = 0; X < 360; X++) {
                const bool InSpan = X >= 72 && X < 120;
                Apart = Apart && (InSpan || Other.Row(Y)[X].Alpha == 0) && (!InSpan || Own.Row(Y)[X].Alpha == 0);
            }
        Count.Check(Apart && Sum(Opacities(Other)) > 50, "each ink drawn alone: a span's glyphs in its ink, the rest in the text's");
    }
    /* Glyphs that overlap, laid once. */
    {
        typecase::Style Close;
        Close.LetterSpacing = -7;
        const auto Text     = Lay(Book, "WWWWW", 30, {}, {}, Close);
        Canvas     Kept     = Make();
        Kept.FillText(Text, {10, 10}, Fill(Color::Rgba(255, 255, 255, 128)));
        double Most = 0;
        for (double A : Opacities(Kept)) Most = std::max(Most, A);
        Count.Check(Most <= 128.5 / 255, Said("glyphs of a line that overlap gathered first and laid once: half opaque "
                                              "ink at most %.3f opaque where they cross",
                                              Most));
    }
    /* Turned, larger than kept, twice the size. */
    {
        const auto Text = Lay(Book, "Turn", 30);
        Canvas     Flat = Make(), Turned = Make(), Twice = *Canvas::Make(360, 240);
        Flat.FillText(Text, {10, 10}, White);
        Turned.FillText(Text, {0, 0}, White, Transform::Rotate(30).Then(Transform::Translate(120, 20)));
        Twice.FillText(Text, {5, 5}, White, Transform::Scale(2));
        const double F = Sum(Opacities(Flat)), T = Sum(Opacities(Turned)), W = Sum(Opacities(Twice));
        const auto   Huge = Lay(Book, "Hg", 300);
        Canvas       Big = *Canvas::Make(400, 400), BigShape = *Canvas::Make(400, 400);
        Big.FillText(Huge, {0, 0}, White);
        BigShape.Fill(TextPath(Huge, {0, 0}), White);
        Count.Check(Near(T, F, 0.02) && Near(W, 4 * F, 0.03) && Opacities(Big) == Opacities(BigShape) && Sum(Opacities(Big)) > 5000,
                    Said("text turned filled as its shape, its ink %.1f of %.1f; twice the size %.1f, four times; larger "
                         "than 256 to the em filled as its shape",
                         T, F, W));
    }
    /* Cut to its box; lines under it; a missing character's box. */
    {
        typecase::TextBox Box;
        Box.Width       = 50;
        Box.Wrap        = false;
        Box.Overflow    = typecase::TextOverflow::Clip;
        const auto Text = Lay(Book, "a line too long", 20, Box);
        Canvas     Cut  = Make();
        Cut.FillText(Text, {20, 10}, White);
        bool Within = true;
        for (std::uint32_t Y = 0; Y < 120; Y++)
            for (std::uint32_t X = 0; X < 360; X++) Within = Within && (Cut.Row(Y)[X].Alpha == 0 || (X >= 20 && X < 70));
        typecase::Style Under;
        Under.Underline   = true;
        const auto Lined  = Lay(Book, "a b", 20, {}, {{0, 3, Under}});
        Canvas     Line   = Make();
        Line.FillText(Lined, {0, 0}, White);
        const auto Row    = static_cast<std::uint32_t>(Lined.Decorations()[0].Of.Y + 0.5f);
        bool       Across = true;
        for (std::uint32_t X = 1; X < 35; X++) Across = Across && Line.Row(Row)[X].Alpha > 60000;
        const auto Missing = Lay(Book, "\xF0\x9F\x98\x80", 20);
        Canvas     Box2    = Make();
        Box2.FillText(Missing, {0, 0}, White);
        Count.Check(Within && Sum(Opacities(Cut)) > 50 && Across && !Missing.Runs()[0].Face && Sum(Opacities(Box2)) > 40,
                    "a text cut by its box kept within it; a line under text, across its space too; a character no "
                    "font has drawn as its box");
    }
    /* Stroked, and casting a shadow, as a shape. */
    {
        const auto Text  = Lay(Book, "Edge", 40);
        Canvas     Plain = Make(), Edged = Make();
        Plain.FillText(Text, {10, 10}, White);
        Stroke Line;
        Line.Width = 2;
        Edged.Stroke(TextPath(Text, {10, 10}), Line, White);
        Shadow Cast;
        const std::optional<Mask> Kept = Cast.MaskFor(TextPath(Text, {10, 10}));
        double                    Shade = 0;
        for (std::int32_t Y = Kept->Area().Y; Y < Kept->Area().Y + Kept->Area().Height; Y++)
            for (std::int32_t X = Kept->Area().X; X < Kept->Area().X + Kept->Area().Width; X++) Shade += Kept->At(X, Y) / 255.0;
        Count.Check(Sum(Opacities(Edged)) > 100 && Near(Shade, Sum(Opacities(Plain)), 0.01),
                    Said("text stroked as a shape is; its shadow its own coverage — %.1f of %.1f", Shade, Sum(Opacities(Plain))));
    }
    /* Made to lean where the family has no italic. */
    {
        typecase::Style Lean;
        Lean.Italic     = true;
        const auto Text = Lay(Book, "lean", 30, {}, {}, Lean);
        Count.Check(!Text.Runs().empty() && Text.Runs()[0].Look.Slant, "a family with no italic face made to lean");
    }
    /* Two strands at once. */
    {
        const auto Text  = Lay(Book, "Two strands, one cache of glyphs", 14);
        const auto Draw  = [&](std::vector<double> &Into) {
            for (int Round = 0; Round < 40; Round++) {
                Canvas C = Make();
                C.FillText(Text, {static_cast<float>(Round % 4) * 0.25f, 30}, White);
                if (Round == 39) Into = Opacities(C);
            }
        };
        std::vector<double> A, B;
        std::thread         One([&] { Draw(A); }), Two([&] { Draw(B); });
        One.join(), Two.join();
        Count.Check(!A.empty() && A == B, "two strands drawing text at once on two canvases, alike");
    }
}

bool WriteTextSheet(const char *Out, const char *FontPath)
{
    typecase::Fonts Book = TreeBook();
    if (FontPath)
        if (auto Face = typecase::Font::Read(Load(FontPath))) {
            const std::string Family = Face->Style().Family;
            Book.Add(std::move(*Face));
            Book.SetDefault(Family);
        }
    const std::uint32_t   W = 1280, H = 860;
    std::optional<Canvas> C = Canvas::Make(W, H);
    C->Clear(Color::Rgb(250, 248, 244));
    const Fill Ink(Color::Rgb(30, 30, 40));
    float      Y = 20;
    for (float Size : {10.0f, 12.0f, 14.0f, 16.0f, 20.0f, 28.0f}) {
        const auto Text = Lay(Book, "The quick brown fox — AVATAR, To, Wa, \xD0\xA1\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB5\xD1\x89\xD1\x91 "
                                    "\xD1\x8D\xD1\x82\xD0\xB8\xD1\x85 \xD0\xBC\xD1\x8F\xD0\xB3\xD0\xBA\xD0\xB8\xD1\x85 "
                                    "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xBE\xD0\xBA",
                              Size);
        C->FillText(Text, {20, Y}, Ink);
        Y += Text.Height() + 6;
    }
    typecase::Style Bold, Italic, Both, Under, Strike, Red, Spaced;
    Bold.Weight = 700, Italic.Italic = true, Both.Weight = 700, Both.Italic = true, Under.Underline = true;
    Strike.Strike = true, Red.Ink = 1, Spaced.LetterSpacing = 3;
    const auto Styled = Lay(Book, "Regular, bold, italic, bold italic, underlined, struck, red, s p a c e d.", 20, {},
                            {{9, 13, Bold}, {15, 21, Italic}, {23, 34, Both}, {36, 46, Under}, {48, 54, Strike},
                             {56, 59, Red}, {61, 67, Spaced}});
    C->FillText(Styled, {20, Y}, Ink, {}, 1.0f, 0);
    C->FillText(Styled, {20, Y}, Fill(Color::Rgb(200, 40, 50)), {}, 1.0f, 1);
    Y += Styled.Height() + 14;

    const char *Paragraph =
        "Typecase lays text in a box: lines broken between words, aligned left, in the middle, right or justified, "
        "a word too long broken between its characters, and what does not fit cut off with an ellipsis\xE2\x80\xA6 "
        "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9 \xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 "
        "\xD1\x82\xD0\xBE\xD0\xB6\xD0\xB5.";
    const typecase::TextAlign Aligns[4] = {typecase::TextAlign::Left, typecase::TextAlign::Center, typecase::TextAlign::Right,
                                           typecase::TextAlign::Justify};
    for (int K = 0; K < 4; K++) {
        typecase::TextBox Box;
        Box.Width       = 290;
        Box.Align       = Aligns[K];
        Box.MaxLines    = 5;
        Box.Overflow    = typecase::TextOverflow::Ellipsis;
        const auto Text = Lay(Book, Paragraph, 14, Box);
        const float X   = 20 + 310.0f * static_cast<float>(K);
        C->Fill(Path::Rect({X - 4, Y - 4, 298, Text.Height() + 8}), Fill(Color::Rgb(236, 232, 226)));
        C->FillText(Text, {X, Y}, Ink);
    }
    Y += 120;

    const auto Big = Lay(Book, "Gradient & glow", 64);
    Shadow     Glow;
    Glow.Blur  = 18, Glow.Color = Color::Rgba(120, 60, 200, 140), Glow.Offset = {0, 6};
    C->DrawShadow(*Glow.MaskFor(TextPath(Big, {20, Y})), Glow);
    C->FillText(Big, {20, Y},
                Fill(Gradient::Linear({20, 0}, {560, 0}, {{0, Color::Rgb(250, 90, 120)}, {1, Color::Rgb(80, 120, 250)}})));
    const auto Hollow = Lay(Book, "Stroked", 64);
    Stroke     Line;
    Line.Width = 2;
    C->Stroke(TextPath(Hollow, {640, Y}), Line, Fill(Color::Rgb(30, 30, 40)));
    Y += Big.Height() + 16;
    C->FillText(Lay(Book, "Turned text", 28), {0, 0}, Ink, Transform::Rotate(-8).Then(Transform::Translate(30, Y + 40)));
    C->FillText(Lay(Book, "Missing: \xF0\x9F\x98\x80 \xE4\xB8\xAD", 28), {420, Y + 10}, Ink);

    std::vector<std::uint32_t> Glass(static_cast<std::size_t>(W) * H);
    C->CopyOut(Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)}, Glass.data(), W, BOX_PIXEL_BGR, Dither::On);
    std::FILE *File = std::fopen(Out, "wb");
    if (!File) return false;
    std::fprintf(File, "P6\n%u %u\n255\n", W, H);
    for (std::uint32_t Pixel : Glass) {
        const unsigned char Rgb[3] = {static_cast<unsigned char>(Pixel >> 16), static_cast<unsigned char>(Pixel >> 8),
                                      static_cast<unsigned char>(Pixel)};
        std::fwrite(Rgb, 1, 3, File);
    }
    return std::fclose(File) == 0;
}

}  // namespace stand
