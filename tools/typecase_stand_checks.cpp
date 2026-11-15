/* typecase stand — the rasterizer held to exact areas and to a fine
 * sampling of every glyph, and the sources, the chain and the cache held to
 * what they promise. */

#include <cstdint>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "boxos_font.h"
#include "boxos_glyph_fold.h"
#include "typecase/bitmap_source.h"
#include "typecase/chain.h"
#include "typecase/code_box.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/glyph_cache.h"
#include "typecase/rasterizer.h"
#include "typecase_stand.h"

namespace stand {

namespace {

using typecase::Outline;
using typecase::Point;
using typecase::Step;

using Polygon = std::vector<std::pair<double, double>>;

/* A polygon given in pixels, y down, as an outline the rasterizer reads at
 * a scale of one: font units run up, so y is turned. */
Outline AsOutline(const Polygon &Shape)
{
    Outline Out;
    for (std::size_t Index = 0; Index < Shape.size(); Index++) {
        Out.Steps.push_back(Index == 0 ? Step::Move : Step::Line);
        Out.Points.push_back(Point{static_cast<float>(Shape[Index].first), static_cast<float>(-Shape[Index].second)});
    }
    return Out;
}

/* The area of a convex polygon within the pixel at (X, Y): the polygon cut
 * by the pixel's four sides in turn, its area by the shoelace. */
double AreaWithin(Polygon Shape, double X, double Y)
{
    const auto Clip = [](const Polygon &In, auto Inside, auto Cross) {
        Polygon Out;
        for (std::size_t Index = 0; Index < In.size(); Index++) {
            const auto &A = In[Index], &B = In[(Index + 1) % In.size()];
            if (Inside(A)) Out.push_back(A);
            if (Inside(A) != Inside(B)) Out.push_back(Cross(A, B));
        }
        return Out;
    };
    const auto AtX = [](double Side) {
        return [Side](const auto &A, const auto &B) {
            return std::pair{Side, A.second + (B.second - A.second) * (Side - A.first) / (B.first - A.first)};
        };
    };
    const auto AtY = [](double Side) {
        return [Side](const auto &A, const auto &B) {
            return std::pair{A.first + (B.first - A.first) * (Side - A.second) / (B.second - A.second), Side};
        };
    };
    Shape = Clip(Shape, [X](const auto &P) { return P.first >= X; }, AtX(X));
    Shape = Clip(Shape, [X](const auto &P) { return P.first <= X + 1; }, AtX(X + 1));
    Shape = Clip(Shape, [Y](const auto &P) { return P.second >= Y; }, AtY(Y));
    Shape = Clip(Shape, [Y](const auto &P) { return P.second <= Y + 1; }, AtY(Y + 1));
    double Twice = 0;
    for (std::size_t Index = 0; Index < Shape.size(); Index++) {
        const auto &A = Shape[Index], &B = Shape[(Index + 1) % Shape.size()];
        Twice += A.first * B.second - B.first * A.second;
    }
    return std::fabs(Twice) / 2;
}

std::uint8_t At(const typecase::GlyphImage &Image, std::int32_t X, std::int32_t Y)
{
    X -= Image.Left, Y -= Image.Top;
    if (X < 0 || Y < 0 || X >= Image.Width || Y >= Image.Height) return 0;
    return Image.Coverage[static_cast<std::size_t>(Y * Image.Width + X)];
}

/* Each pixel's coverage within one 255th of the convex shape's exact area. */
bool Exact(const typecase::GlyphImage &Image, const Polygon &Shape, typecase::Bounds Over)
{
    for (std::int32_t Y = Over.Top; Y < Over.Bottom; Y++)
        for (std::int32_t X = Over.Left; X < Over.Right; X++) {
            const double Area = std::min(AreaWithin(Shape, X, Y), 1.0);
            if (std::abs(static_cast<int>(At(Image, X, Y)) - static_cast<int>(std::lround(Area * 255))) > 1) {
                std::printf("  pixel (%d,%d): %u, the area is %.4f\n", X, Y, At(Image, X, Y), Area);
                return false;
            }
        }
    return true;
}

Polygon Square(double Left, double Top, double Right, double Bottom, bool Clockwise)
{
    Polygon Out{{Left, Top}, {Right, Top}, {Right, Bottom}, {Left, Bottom}};
    if (!Clockwise) std::reverse(Out.begin(), Out.end());
    return Out;
}

Outline Joined(const std::vector<Polygon> &Shapes)
{
    Outline Out;
    for (const Polygon &Shape : Shapes) {
        const Outline One = AsOutline(Shape);
        Out.Steps.insert(Out.Steps.end(), One.Steps.begin(), One.Steps.end());
        Out.Points.insert(Out.Points.end(), One.Points.begin(), One.Points.end());
    }
    return Out;
}

/* The glyph's coverage sampled 16 x 16 to a pixel — on each sampling row,
 * where the outline's edges cross it, and the winding between — from the
 * outline followed by 64 lines to a curve; and which pixels some sample of
 * finds wound round twice or more, where contours overlap. */
struct Samples {
    std::vector<double> Share;
    std::vector<bool>   Overlap;
};

Samples Sampled(const Outline &Shape, float Scale, const typecase::GlyphImage &Image)
{
    struct Edge {
        double X0, Y0, X1, Y1;
    };
    std::vector<Edge> Edges;
    std::size_t       Next = 0;
    std::pair<double, double> Start{}, Pen{};
    const auto Px = [&](Point P) { return std::pair<double, double>{P.X * Scale, -P.Y * Scale}; };
    const auto Add = [&](std::pair<double, double> A, std::pair<double, double> B) {
        if (A.second != B.second) Edges.push_back(Edge{A.first, A.second, B.first, B.second});
    };
    for (Step Each : Shape.Steps) {
        if (Each == Step::Move) {
            Add(Pen, Start);
            Start = Pen = Px(Shape.Points[Next++]);
        } else if (Each == Step::Line) {
            const auto To = Px(Shape.Points[Next++]);
            Add(Pen, To);
            Pen = To;
        } else {
            const auto From = Pen, C = Px(Shape.Points[Next]), To = Px(Shape.Points[Next + 1]);
            Next += 2;
            for (int Index = 1; Index <= 64; Index++) {
                const double T = Index / 64.0, U = 1 - T;
                const std::pair<double, double> Here{U * U * From.first + 2 * U * T * C.first + T * T * To.first,
                                                     U * U * From.second + 2 * U * T * C.second + T * T * To.second};
                Add(Pen, Here);
                Pen = Here;
            }
            Pen = To;
        }
    }
    Add(Pen, Start);

    Samples Out{std::vector<double>(static_cast<std::size_t>(Image.Width * Image.Height), 0.0),
                std::vector<bool>(static_cast<std::size_t>(Image.Width * Image.Height), false)};
    std::vector<std::pair<double, int>> Crossings;
    for (std::int32_t Row = 0; Row < Image.Height * 16; Row++) {
        const double Y = Image.Top + (Row + 0.5) / 16;
        Crossings.clear();
        for (const Edge &E : Edges) {
            const bool Down = E.Y1 > E.Y0;
            const double Low = Down ? E.Y0 : E.Y1, High = Down ? E.Y1 : E.Y0;
            if (Y < Low || Y >= High) continue;
            Crossings.push_back({E.X0 + (E.X1 - E.X0) * (Y - E.Y0) / (E.Y1 - E.Y0), Down ? 1 : -1});
        }
        std::sort(Crossings.begin(), Crossings.end());
        std::size_t Passed  = 0;
        int         Winding = 0;
        for (std::int32_t Column = 0; Column < Image.Width * 16; Column++) {
            const double X = Image.Left + (Column + 0.5) / 16;
            while (Passed < Crossings.size() && Crossings[Passed].first < X) Winding += Crossings[Passed++].second;
            const auto Pixel = static_cast<std::size_t>((Row / 16) * Image.Width + Column / 16);
            if (Winding != 0) Out.Share[Pixel] += 1.0 / 256;
            if (std::abs(Winding) > 1) Out.Overlap[Pixel] = true;
        }
    }
    return Out;
}

/* The digits DrawCodeBox cut, read back off the picture at the scale and
 * layout it chose — whether a person could read the code. */
std::string ReadCode(const typecase::GlyphImage &Image, int Count, int Columns, int Times)
{
    static const std::uint8_t Digits[16][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
        {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
        {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7}, {7, 4, 7, 4, 4},
    };
    const int Rows = Count / Columns;
    const int Left = (Image.Width - (Columns * 4 * Times - Times)) / 2;
    const int Top  = (Image.Height - (Rows * 6 * Times - Times)) / 2;
    std::string Said;
    for (int Index = 0; Index < Count; Index++) {
        std::uint8_t Seen[5] = {};
        for (int Y = 0; Y < 5; Y++)
            for (int X = 0; X < 3; X++)
                if (Image.Coverage[static_cast<std::size_t>((Top + (Index / Columns) * 6 * Times + Y * Times) * Image.Width +
                                                            Left + (Index % Columns) * 4 * Times + X * Times)] == 0)
                    Seen[Y] = static_cast<std::uint8_t>(Seen[Y] | (4 >> X));
        int Digit = -1;
        for (int Each = 0; Each < 16; Each++)
            if (std::equal(Seen, Seen + 5, Digits[Each])) Digit = Each;
        Said += Digit < 0 ? '?' : "0123456789ABCDEF"[Digit];
    }
    return Said;
}

}  // namespace

static std::optional<std::uint32_t> BoxFontIndex(char32_t Character)
{
    if (Character < 0x20 || Character == 0x7F) return std::nullopt;
    char Cells[BOXOS_GLYPH_FOLD_MAX];
    if (BoxGlyphFoldCodepoint(Character, Cells) != 1) return std::nullopt;
    const auto Glyph = static_cast<unsigned char>(Cells[0]);
    if (Glyph == static_cast<unsigned char>(BOXOS_GLYPH_FOLD_BOX) && Character != U'■') return std::nullopt;
    return Glyph;
}

std::unique_ptr<typecase::BitmapSource> BoxFont()
{
    return std::make_unique<typecase::BitmapSource>(
        std::span<const std::uint8_t>(&BoxFontGlyphs[0][0], sizeof BoxFontGlyphs), BOXOS_FONT_WIDTH,
        BOXOS_FONT_HEIGHT, BoxFontIndex);
}

void CheckRaster(Tally &Count)
{
    typecase::Rasterizer   Pen;
    const typecase::Bounds Wide{-64, -64, 64, 64};
    std::mt19937           Random(20261115);
    std::uniform_real_distribution<double> Where(0.3, 12.0), Span(0.05, 7.0);

    int Held = 0;
    for (int Shape = 0; Shape < 400; Shape++) {
        Polygon Of;
        if (Shape % 2) {
            const double X = Where(Random), Y = Where(Random);
            Of = Square(X, Y, X + Span(Random), Y + Span(Random), Shape % 4 == 1);
        } else {
            Of = {{Where(Random), Where(Random)}, {Where(Random), Where(Random)}, {Where(Random), Where(Random)}};
        }
        const auto Image = Pen.Draw(AsOutline(Of), 1, 1, Wide, 0);
        if (Exact(Image, Of, typecase::Bounds{-1, -1, 21, 21})) Held++;
        else std::printf("  shape %d\n", Shape);
    }
    Count.Check(Held == 400, "400 rectangles and triangles, every pixel within 1/255 of its exact area (" +
                                 std::to_string(Held) + " held)");

    const Polygon Long = Square(-30, 2.25, 50, 4.5, true);
    const auto    Kept = Pen.Draw(AsOutline(Long), 1, 1, typecase::Bounds{0, 0, 10, 10}, 0);
    Count.Check(Kept.Left == 0 && Kept.Width == 10 && Exact(Kept, Long, typecase::Bounds{0, 0, 10, 10}),
                "a shape far past the limit: the picture kept to it, each pixel inside still exact");

    const auto Both = Pen.Draw(Joined({Square(1, 1, 7, 7, true), Square(4, 4, 10, 10, true)}), 1, 1, Wide, 0);
    bool Once = true;
    for (std::int32_t Y = 0; Y < 12; Y++)
        for (std::int32_t X = 0; X < 12; X++) {
            const bool Inside = (X >= 1 && X < 7 && Y >= 1 && Y < 7) || (X >= 4 && X < 10 && Y >= 4 && Y < 10);
            Once = Once && At(Both, X, Y) == (Inside ? 255 : 0);
        }
    Count.Check(Once, "two contours wound the same way, overlapping: covered once, their overlap whole");

    const auto Ring = Pen.Draw(Joined({Square(1, 1, 11, 11, true), Square(4, 4, 8, 8, false)}), 1, 1, Wide, 0);
    const auto Twice = Pen.Draw(Joined({Square(1, 1, 11, 11, true), Square(4, 4, 8, 8, true)}), 1, 1, Wide, 0);
    Count.Check(At(Ring, 5, 5) == 0 && At(Ring, 2, 2) == 255 && At(Twice, 5, 5) == 255,
                "a contour wound against its outer one is a hole; wound with it, ink");

    /* A shape of curves: its area is the polygon of its points on the curve
     * and two thirds of each curve's triangle. Lines that keep within 1/16
     * of a pixel of a curve cut off at most two thirds of that along its
     * length — no more than along its controls' — and the coverage summed
     * must be the area to that. */
    Outline Lens;
    Lens.Steps  = {Step::Move, Step::Quad, Step::Quad};
    Lens.Points = {Point{2, -10}, Point{12, -1}, Point{22, -10}, Point{12, -19}, Point{2, -10}};
    const auto Drawn = Pen.Draw(Lens, 1, 1, Wide, 0);
    double     Sum   = 0;
    for (std::uint8_t Each : Drawn.Coverage) Sum += Each / 255.0;
    const double Exact = 2.0 / 3 * 20 * 9 * 2 / 2, Length = 4 * std::hypot(10.0, 9.0);
    Count.Check(std::fabs(Sum - Exact) <= 2.0 / 3 / 16 * Length,
                "a lens of two curves: its coverage sums to its area, short by no more than its curves followed by "
                "lines may be (" + std::to_string(Sum) + " of " + std::to_string(Exact) + ")");
}

void CheckGlyphs(Tally &Count, const std::vector<std::uint8_t> &Bytes)
{
    const auto Read = typecase::Font::Read(Bytes);
    if (!Read) return Count.Check(false, "the font reads: " + Read.error().Said);
    typecase::Rasterizer Pen;
    for (const float Em : {13.333333f, 32.0f}) {
        const float Scale = Em / Read->Metrics().UnitsPerEm;
        double        Total = 0, Worst = 0, WorstOverlap = 0;
        std::size_t   Pixels = 0;
        std::uint32_t WorstGlyph = 0;
        for (std::uint32_t Glyph = 0; Glyph < Read->GlyphCount(); Glyph++) {
            const auto Shape = Read->OutlineOf(static_cast<typecase::GlyphId>(Glyph));
            if (!Shape) continue;
            const auto Image = Pen.Draw(*Shape, Scale, Scale, typecase::Bounds{-256, -256, 256, 256}, 0);
            const auto Truth = Sampled(*Shape, Scale, Image);
            for (std::size_t Index = 0; Index < Truth.Share.size(); Index++) {
                const double Off = std::fabs(Image.Coverage[Index] / 255.0 - Truth.Share[Index]);
                Total += Off;
                if (Truth.Overlap[Index]) WorstOverlap = std::max(WorstOverlap, Off);
                else if (Off > Worst) Worst = Off, WorstGlyph = Glyph;
            }
            Pixels += Truth.Share.size();
        }
        const double Mean = Total / static_cast<double>(Pixels);
        char Said[200];
        std::snprintf(Said, sizeof Said,
                      "every glyph at %.1f px to the em against 16x16 sampling: mean %.5f, worst %.3f (glyph %u), "
                      "where contours overlap %.3f, over %zu pixels",
                      static_cast<double>(Em), Mean, Worst, WorstGlyph, WorstOverlap, Pixels);
        /* Sampling 16 to a pixel's width is itself up to 1/32 off at an
         * upright edge and lines along a curve may cut off 1/24 of a pixel:
         * a pixel within 0.12 of the samples, and on the whole within 0.012
         * — an edge out by a tenth of a pixel is more. Where contours
         * overlap a pixel may take more than their union (rasterizer.h),
         * by less than the smaller share: under a half. */
        Count.Check(Mean < 0.012 && Worst < 0.12 && WorstOverlap < 0.5, Said);
    }
}

void CheckSources(Tally &Count, const std::vector<std::uint8_t> &Bytes)
{
    auto Read = typecase::Font::Read(Bytes);
    if (!Read) return Count.Check(false, "the font reads: " + Read.error().Said);
    const typecase::Size Cell = typecase::FitCell(*Read, 8, 16);
    Count.Check(std::fabs(Cell.Em - 8.0f * Read->Metrics().UnitsPerEm / Read->AdvanceOf(Read->Find(U'M'))) < 1e-3f &&
                    Cell.Baseline > 8 && Cell.Baseline < 16,
                "a cell of 8 by 16: M's advance its width, the baseline in its lower half (" +
                    std::to_string(Cell.Baseline) + ")");

    typecase::FontSource Face(std::move(*Read));
    bool Within = true, Said = true;
    for (char32_t Character = 0x20; Character < 0x500; Character++) {
        if (!Face.Has(Character)) continue;
        const auto Image = Face.Draw(Character, Cell);
        Said   = Said && Image.has_value();
        Within = Within && Image && Image->Left >= -14 && Image->Left + Image->Width <= 8 + 14 &&
                 Image->Top >= -Cell.Baseline - 14 && Image->Top + Image->Height <= 16 - Cell.Baseline + 14;
    }
    Count.Check(Said && Within, "every character the font has, U+0020..U+04FF, drawn and kept within the cell and an em");
    Count.Check(!Face.Has(U'一') && !Face.Draw(U'一', Cell), "a character the font has not: not had, not drawn");

    bool Sharp = true;
    std::string Tops;
    for (const std::int32_t Wide : {8, 10, 12, 16}) {
        const typecase::Size At    = typecase::FitCell(Face.Of(), Wide, Wide * 2);
        const float          Lower = Face.Of().Metrics().XHeight * At.Em / Face.Of().Metrics().UnitsPerEm;
        const auto           Z     = Face.Draw(U'z', At);
        const auto           Top   = std::max_element(Z->Coverage.begin(), Z->Coverage.begin() + Z->Width);
        Sharp = Sharp && Z->Top == -static_cast<std::int32_t>(std::lround(Lower)) && *Top >= 250;
        Tops += " " + std::to_string(Wide) + ":" + std::to_string(*Top);
    }
    Count.Check(Sharp, "the lowercase a whole number of pixels high: z's top row ends on its pixel edge, inked whole "
                       "(the most of its first row, by cell width:" + Tops + ")");

    auto Bitmap = BoxFont();
    const typecase::Size Small{0, 8, 16, 12}, Large{0, 16, 32, 24}, Odd{0, 12, 24, 18};
    const auto A = Bitmap->Draw(U'A', Small), A2 = Bitmap->Draw(U'A', Large), A3 = Bitmap->Draw(U'A', Odd);
    bool Same = A && A2 && A3 && A->Width == 8 && A->Height == 16 && A->Top == -12;
    for (int Y = 0; Same && Y < 16; Y++)
        for (int X = 0; X < 8; X++) {
            const bool Ink = BoxFontGlyphs['A'][Y] & (0x80 >> X);
            Same = Same && A->Coverage[static_cast<std::size_t>(Y * 8 + X)] == (Ink ? 255 : 0) &&
                   A2->Coverage[static_cast<std::size_t>((2 * Y + 1) * 16 + 2 * X)] == (Ink ? 255 : 0) &&
                   A3->Coverage[static_cast<std::size_t>((Y + 4) * 12 + X + 2)] == (Ink ? 255 : 0);
        }
    Count.Check(Same, "the bitmap font's A: as its rows, doubled in a cell twice its size, centred in one between");
    Count.Check(Bitmap->Has(U'█') && !Bitmap->Has(U'一') && !Bitmap->Has(U'\x07') && Bitmap->Has(U'■'),
                "the bitmap font has what it folds to one glyph of its own, and ■ itself; no control, no box for another");

    const auto Box = typecase::DrawCodeBox(U'一', Small);
    Count.Check(Box.Width == 8 && Box.Height == 16 && ReadCode(Box, 4, 2, 1) == "4E00",
                "U+4E00 in a cell of 8 by 16: its code reads back off the picture, 4E00");
    const auto Far = typecase::DrawCodeBox(U'\U0001F600', Small);
    const auto Filled  = [](const typecase::GlyphImage &I) {
        return std::all_of(I.Coverage.begin(), I.Coverage.end(), [](std::uint8_t V) { return V == 255; });
    };
    const auto Natural = typecase::DrawCodeBox(U'\U0001F600', typecase::Size{0, 8, 18, 14});
    const auto Roomy   = typecase::DrawCodeBox(U'\U0001F600', typecase::Size{0, 10, 20, 15});
    Count.Check(Filled(Far) && Filled(Natural) && ReadCode(Roomy, 6, 2, 1) == "01F600",
                "U+1F600: six digits fit neither 8 by 16 nor 8 by 18 with ink round them, the cell is filled; in 10 "
                "by 20 they read back, 01F600");
    Count.Check(ReadCode(typecase::DrawCodeBox(U'ꯍ', Large), 4, 2, 2) == "ABCD",
                "in a cell of 16 by 32 the digits are twice the size and read back, ABCD");

    typecase::Chain Cells;
    Cells.Add(BoxFont());
    Cells.Add(std::make_unique<typecase::FontSource>(std::move(*typecase::Font::Read(Bytes))));
    typecase::Chain Text;
    Text.Add(std::make_unique<typecase::FontSource>(std::move(*typecase::Font::Read(Bytes))));
    Text.Add(BoxFont());
    const auto ByBitmap = Cells.Draw(U'A', Cell), ByFont = Text.Draw(U'A', Cell), ByNone = Text.Draw(U'一', Cell);
    const auto Smooth   = [](const typecase::GlyphImage &I) {
        return std::any_of(I.Coverage.begin(), I.Coverage.end(), [](std::uint8_t V) { return V > 0 && V < 255; });
    };
    Count.Check(!Smooth(ByBitmap) && Smooth(ByFont) && ReadCode(ByNone, 4, 2, 1) == "4E00" && Text.Has(U'A') &&
                    !Text.Has(U'一'),
                "a chain asks its sources in order — the first that has a character draws it — and draws what none "
                "has as its code");

    typecase::Chain Screen;
    Screen.Add(std::make_unique<typecase::GeometrySource>());
    Screen.Add(std::make_unique<typecase::FontSource>(std::move(*typecase::Font::Read(Bytes))));
    typecase::GeometrySource Lines;
    const auto Frame = Screen.Draw(U'\u2502', Cell), Drawn = *Lines.Draw(U'\u2502', Cell);
    Count.Check(Frame.Coverage == Drawn.Coverage && Frame.Width == Cell.Width && Screen.Draw(U'A', Cell).Width < Cell.Width + 2,
                "a screen's chain, geometry before the font: a frame's line from geometry, to the cell; a letter from the "
                "font");

    typecase::GlyphCache Kept(Text, 4096);
    const auto First = Kept.Get(U'A', Cell).Coverage;
    for (char32_t Character = U'B'; Character <= U'Z'; Character++) (void)Kept.Get(Character, Cell);
    const bool Bounded = Kept.Held() <= 4096;
    const auto Again   = Kept.Get(U'A', Cell).Coverage;
    Kept.Clear();
    Count.Check(Bounded && Again == First && Kept.Held() == 0,
                "the cache keeps within its budget, draws again what it let go as it was, and empties");
}

void Tally::Check(bool Holds, const std::string &What)
{
    (Holds ? Held : Failed)++;
    std::printf("%s %s\n", Holds ? "held" : "FAILED", What.c_str());
}

}  // namespace stand
