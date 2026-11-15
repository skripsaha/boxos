/* typecase stand — geometry held to its contract (geometry_source.h): every
 * character it has drawn as the whole cell at every size; each frame line
 * leaving the cell at the pixels its weight takes there and nowhere else,
 * so a frame closes cell to cell; every mosaic and its complement filling
 * the cell exactly once, sub-cell by sub-cell; the eighths on the octants'
 * edges; the shades a quarter, a half and three quarters; braille's dots
 * where they stand; each wedge the cell's side of its diagonal its name
 * says, and with its complement the cell once. */

#include <cstdint>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "boxos_mosaic.h"
#include "typecase/geometry_source.h"
#include "typecase_stand.h"

namespace stand {

namespace {

constexpr std::int32_t Sizes[][2] = {{5, 10}, {6, 12}, {7, 14}, {8, 16}, {9, 18}, {10, 20},
                                     {11, 23}, {12, 24}, {16, 32}, {24, 48}};

std::int32_t EdgeOf(std::int32_t Index, std::int32_t Parts, std::int32_t Length)
{
    return (Length * Index + Parts / 2) / Parts;
}

/* The arms of the frame characters drawn as arms (not the dashes, arcs and
 * diagonals), as their Unicode names say: U+XXXX then left, up, right,
 * down — 0 none, 1 light, 2 heavy, 3 double. */
struct Arms {
    int Left, Up, Right, Down;
};

std::vector<std::pair<char32_t, Arms>> PlainArms()
{
    const char *Encoded =
        "25001010 25012020 25020101 25030202 250C0011 250D0021 250E0012 250F0022 25101001 25112001 25121002 "
        "25132002 25140110 25150120 25160210 25170220 25181100 25192100 251A1200 251B2200 251C0111 251D0121 "
        "251E0211 251F0112 25200212 25210221 25220122 25230222 25241101 25252101 25261201 25271102 25281202 "
        "25292201 252A2102 252B2202 252C1011 252D2011 252E1021 252F2021 25301012 25312012 25321022 25332022 "
        "25341110 25352110 25361120 25372120 25381210 25392210 253A1220 253B2220 253C1111 253D2111 253E1121 "
        "253F2121 25401211 25411112 25421212 25432211 25441221 25452112 25461122 25472221 25482122 25492212 "
        "254A1222 254B2222 25503030 25510303 25520031 25530013 25540033 25553001 25561003 25573003 25580130 "
        "25590310 255A0330 255B3100 255C1300 255D3300 255E0131 255F0313 25600333 25613101 25621303 25633303 "
        "25643031 25651013 25663033 25673130 25681310 25693330 256A3131 256B1313 256C3333 25741000 25750100 "
        "25760010 25770001 25782000 25790200 257A0020 257B0002 257C1020 257D0102 257E2010 257F0201";
    std::vector<std::pair<char32_t, Arms>> Out;
    for (const char *At = Encoded; *At;) {
        unsigned Code = 0;
        int      L, U, R, D, Used = 0;
        if (std::sscanf(At, "%4x%1d%1d%1d%1d%n", &Code, &L, &U, &R, &D, &Used) != 5) break;
        At += Used;
        while (*At == ' ') At++;
        Out.push_back({static_cast<char32_t>(Code), Arms{L, U, R, D}});
    }
    return Out;
}

/* The pixels of one axis an arm of Weight takes where it leaves the cell. */
std::vector<bool> Strokes(int Weight, std::int32_t Length, std::int32_t Light)
{
    std::vector<bool> Out(static_cast<std::size_t>(Length), false);
    const auto Mark = [&](std::int32_t From, std::int32_t Thick) {
        for (std::int32_t Index = From; Index < From + Thick; Index++) Out[static_cast<std::size_t>(Index)] = true;
    };
    if (Weight == 1) Mark((Length - Light) / 2, Light);
    if (Weight == 2) Mark((Length - 2 * Light) / 2, 2 * Light);
    if (Weight == 3) {
        const std::int32_t Low = (Length - 3 * Light) / 2;
        Mark(Low, Light);
        Mark(Low + 2 * Light, Light);
    }
    return Out;
}

/* The wedges U+1FB3C..U+1FB67 as their Unicode names say: the corner the
 * part holds, then the two ends of its diagonal, each a column in halves (0
 * to 2) and a row in thirds (0 to 3) of the cell. */
struct Wedge {
    char32_t Character;
    int      Corner[2], From[2], To[2];
};

std::vector<Wedge> Wedges()
{
    const char *Encoded =
        "1FB3C030213 1FB3D030223 1FB3E030113 1FB3F030123 1FB40030013 1FB41230110 1FB42230120 1FB43230210 "
        "1FB44230220 1FB45230310 1FB46230221 1FB47231322 1FB48230322 1FB49231321 1FB4A230321 1FB4B231320 "
        "1FB4C031021 1FB4D030021 1FB4E031022 1FB4F030022 1FB50031023 1FB51030122 1FB52200213 1FB53200223 "
        "1FB54200113 1FB55200123 1FB56200013 1FB57000110 1FB58000120 1FB59000210 1FB5A000220 1FB5B000310 "
        "1FB5C000221 1FB5D001322 1FB5E000322 1FB5F001321 1FB60000321 1FB61001320 1FB62201021 1FB63200021 "
        "1FB64201022 1FB65200022 1FB66201023 1FB67200122";
    std::vector<Wedge> Out;
    for (const char *At = Encoded; *At;) {
        unsigned Code = 0;
        Wedge    One{};
        int      Used = 0;
        if (std::sscanf(At, "%5x%1d%1d%1d%1d%1d%1d%n", &Code, &One.Corner[0], &One.Corner[1], &One.From[0],
                        &One.From[1], &One.To[0], &One.To[1], &Used) != 7)
            break;
        One.Character = static_cast<char32_t>(Code);
        Out.push_back(One);
        At += Used;
        while (*At == ' ') At++;
    }
    return Out;
}

/* How far, in pixels, the middle of pixel X, Y lies from the line A..B, on
 * the side positive when it is the side of C. */
float SideOf(float Ax, float Ay, float Bx, float By, float Cx, float Cy, std::int32_t X, std::int32_t Y)
{
    const float Dx = Bx - Ax, Dy = By - Ay, Length = std::hypot(Dx, Dy);
    const auto  Cross = [&](float Px, float Py) { return Dx * (Py - Ay) - Dy * (Px - Ax); };
    const float Here  = Cross(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f) / Length;
    return Cross(Cx, Cy) >= 0 ? Here : -Here;
}

bool Whole(const typecase::GlyphImage &Image, const typecase::Size &At)
{
    return Image.Left == 0 && Image.Top == -At.Baseline && Image.Width == At.Width && Image.Height == At.Height &&
           Image.Coverage.size() == static_cast<std::size_t>(At.Width * At.Height);
}

std::uint8_t Pixel(const typecase::GlyphImage &Image, std::int32_t X, std::int32_t Y)
{
    return Image.Coverage[static_cast<std::size_t>(Y * Image.Width + X)];
}

}  // namespace

void CheckGeometry(Tally &Count)
{
    typecase::GeometrySource Geometry;
    std::vector<char32_t>    Every;
    for (char32_t Each = 0x2500; Each <= 0x259F; Each++) Every.push_back(Each);
    for (char32_t Each = 0x2800; Each <= 0x28FF; Each++) Every.push_back(Each);
    for (char32_t Each = 0x1FB3C; Each <= 0x1FB6F; Each++) Every.push_back(Each);
    for (const BoxMosaicShape &Each : BoxMosaicShapes)
        if (Each.Character != U' ' && (Each.Character < 0x2580 || Each.Character > 0x259F)) Every.push_back(Each.Character);

    bool Had = true, Drawn = true;
    for (char32_t Each : Every) Had = Had && Geometry.Has(Each);
    for (const auto &[Wide, Tall] : Sizes)
        for (char32_t Each : Every) {
            const typecase::Size At{0, Wide, Tall, Tall * 3 / 4};
            const auto           Image = Geometry.Draw(Each, At);
            Drawn = Drawn && Image && Whole(*Image, At);
        }
    Count.Check(Had && !Geometry.Has(U' ') && !Geometry.Has(U'A') && !Geometry.Has(U'☀'),
                "geometry has its " + std::to_string(Every.size()) + " characters — frames, blocks, mosaics, wedges, braille "
                "— and no other, not the space");
    Count.Check(Drawn, "each drawn as the whole cell, at ten sizes from 5 by 10 to 24 by 48, odd ones among them");

    /* Every frame line leaves the cell at its weight's pixels, and no ink
     * touches an edge it has no arm to. */
    int  Lines = 0;
    bool Meet  = true;
    for (const auto &[Character, Of] : PlainArms()) {
        Lines++;
        for (const auto &[Wide, Tall] : Sizes) {
            const typecase::Size At{0, Wide, Tall, Tall * 3 / 4};
            const auto           Image = *Geometry.Draw(Character, At);
            const std::int32_t   Light = std::max(1, (Wide + 5) / 10);
            const auto Rows = [&](int Weight) { return Strokes(Weight, Tall, Light); };
            const auto Columns = [&](int Weight) { return Strokes(Weight, Wide, Light); };
            for (std::int32_t Y = 0; Y < Tall; Y++) {
                Meet = Meet && (Pixel(Image, 0, Y) == 255) == Rows(Of.Left)[static_cast<std::size_t>(Y)];
                Meet = Meet && (Pixel(Image, Wide - 1, Y) == 255) == Rows(Of.Right)[static_cast<std::size_t>(Y)];
            }
            for (std::int32_t X = 0; X < Wide; X++) {
                Meet = Meet && (Pixel(Image, X, 0) == 255) == Columns(Of.Up)[static_cast<std::size_t>(X)];
                Meet = Meet && (Pixel(Image, X, Tall - 1) == 255) == Columns(Of.Down)[static_cast<std::size_t>(X)];
            }
            if (!Meet) {
                std::printf("  U+%04X at %d by %d leaves the cell off its weight's pixels\n",
                            static_cast<unsigned>(Character), Wide, Tall);
                break;
            }
        }
        if (!Meet) break;
    }
    Count.Check(Meet && Lines == 109, "each of " + std::to_string(Lines) + " frame characters leaves the cell at the "
                                     "pixels its arms' weights take there, light, heavy and double, and no other — at "
                                     "every size");

    bool Tiled = true;
    for (const auto &[Wide, Tall] : Sizes) {
        const typecase::Size At{0, Wide, Tall, Tall * 3 / 4};
        for (const auto &[Rows, Table, Patterns] : {std::tuple{2, BoxMosaicQuadrants, 16},
                                                    std::tuple{3, BoxMosaicSextants, 64},
                                                    std::tuple{4, BoxMosaicOctants, 256}}) {
            for (int Pattern = 0; Pattern < Patterns && Tiled; Pattern++) {
                const auto Ink = [&](int Of) {
                    return Table[Of] == U' ' ? std::vector<std::uint8_t>(static_cast<std::size_t>(Wide * Tall), 0)
                                             : Geometry.Draw(Table[Of], At)->Coverage;
                };
                const auto Mine = Ink(Pattern), Rest = Ink((Patterns - 1) ^ Pattern);
                for (std::int32_t Y = 0; Y < Tall && Tiled; Y++)
                    for (std::int32_t X = 0; X < Wide; X++) {
                        int Row = 0;
                        while (Y >= EdgeOf(Row + 1, Rows, Tall)) Row++;
                        const int Column = X >= EdgeOf(1, 2, Wide) ? 1 : 0;
                        const bool Set   = Pattern & (1 << (Row * 2 + Column));
                        const auto Here  = static_cast<std::size_t>(Y * Wide + X);
                        Tiled = Tiled && Mine[Here] == (Set ? 255 : 0) && Mine[Here] + Rest[Here] == 255;
                    }
                if (!Tiled) std::printf("  pattern %d of %d rows at %d by %d\n", Pattern, Rows, Wide, Tall);
            }
        }
    }
    Count.Check(Tiled, "every pattern of quadrants, sextants and octants fills exactly its sub-cells, and with its "
                       "complement the whole cell once — at every size");

    bool Eighths = true, Shades = true, Dots = true;
    for (const auto &[Wide, Tall] : Sizes) {
        const typecase::Size At{0, Wide, Tall, Tall * 3 / 4};
        for (int Parts = 1; Parts < 8; Parts++) {
            const auto Lower = Geometry.Draw(static_cast<char32_t>(0x2580 + Parts), At);
            for (std::int32_t Y = 0; Y < Tall; Y++)
                Eighths = Eighths && (Pixel(*Lower, 0, Y) == 255) == (Y >= EdgeOf(8 - Parts, 8, Tall));
            const auto Left = Geometry.Draw(static_cast<char32_t>(0x2590 - Parts), At);
            for (std::int32_t X = 0; X < Wide; X++)
                Eighths = Eighths && (Pixel(*Left, X, 0) == 255) == (X < EdgeOf(Parts, 8, Wide));
        }
        if (Wide % 2 == 0 && Tall % 2 == 0)
            for (int Level = 0; Level < 3; Level++) {
                const auto Shade = Geometry.Draw(static_cast<char32_t>(0x2591 + Level), At);
                const auto Inked = std::count(Shade->Coverage.begin(), Shade->Coverage.end(), 255);
                Shades = Shades && Inked * 4 == static_cast<long>(Wide * Tall) * (Level + 1);
            }
        for (int Pattern = 0; Pattern < 256; Pattern++) {
            constexpr int Columns[8] = {0, 0, 0, 1, 1, 1, 0, 1}, Rows[8] = {0, 1, 2, 0, 1, 2, 3, 3};
            const auto Braille = Geometry.Draw(static_cast<char32_t>(0x2800 + Pattern), At);
            for (int Dot = 0; Dot < 8; Dot++) {
                bool Inked = false;
                for (std::int32_t Y = EdgeOf(Rows[Dot], 4, Tall); Y < EdgeOf(Rows[Dot] + 1, 4, Tall); Y++)
                    for (std::int32_t X = EdgeOf(Columns[Dot], 2, Wide); X < EdgeOf(Columns[Dot] + 1, 2, Wide); X++)
                        Inked = Inked || Pixel(*Braille, X, Y) > 0;
                Dots = Dots && Inked == ((Pattern >> Dot & 1) != 0);
            }
        }
    }
    Count.Check(Eighths, "the eighths from the bottom and from the left end on the edges the mosaics' rows and columns "
                         "end on");
    Count.Check(Shades, "the shades ink a quarter, a half and three quarters of the cell's pixels");
    Count.Check(Dots, "braille's 256 patterns: ink in the part of the cell of each dot raised, and in no other");

    /* Each wedge inks the side of its diagonal its corner is on and nothing
     * of the other, away from the diagonal by more than the pixel it may
     * share; each triangle the part between its side and the middle. And a
     * wedge with its complement — the 22 after it, a quarter's three
     * quarters — shares every pixel to within the rounding of either. */
    bool Sided = true, Paired = true;
    for (const auto &[Wide, Tall] : Sizes) {
        const typecase::Size At{0, Wide, Tall, Tall * 3 / 4};
        const auto X = [&](int Column) { return static_cast<float>(EdgeOf(Column, 2, Wide)); };
        const auto Y = [&](int Row) { return static_cast<float>(EdgeOf(Row, 3, Tall)); };
        const auto Ink = [&](char32_t Character) { return Geometry.Draw(Character, At)->Coverage; };
        for (const Wedge &Each : Wedges()) {
            const auto Shown = Ink(Each.Character);
            for (std::int32_t Py = 0; Py < Tall; Py++)
                for (std::int32_t Px = 0; Px < Wide; Px++) {
                    const float Side = SideOf(X(Each.From[0]), Y(Each.From[1]), X(Each.To[0]), Y(Each.To[1]),
                                              X(Each.Corner[0]), Y(Each.Corner[1]), Px, Py);
                    if (std::fabs(Side) < 0.75f) continue;
                    Sided = Sided && Shown[static_cast<std::size_t>(Py * Wide + Px)] == (Side > 0 ? 255 : 0);
                }
        }
        const float Mx = X(1), My = static_cast<float>(EdgeOf(1, 2, Tall)), Right = X(2), Foot = static_cast<float>(Tall);
        const float Ends[4][4] = {{0, Foot, 0, 0}, {0, 0, Right, 0}, {Right, 0, Right, Foot}, {Right, Foot, 0, Foot}};
        for (int Side = 0; Side < 4; Side++) {
            const auto  Shown = Ink(static_cast<char32_t>(0x1FB6C + Side));
            const float *End  = Ends[Side];
            const float Cx = (End[0] + End[2]) / 2, Cy = (End[1] + End[3]) / 2;   // the middle of its side
            for (std::int32_t Py = 0; Py < Tall; Py++)
                for (std::int32_t Px = 0; Px < Wide; Px++) {
                    const float One = SideOf(End[0], End[1], Mx, My, Cx, Cy, Px, Py);
                    const float Two = SideOf(Mx, My, End[2], End[3], Cx, Cy, Px, Py);
                    if (std::fabs(One) < 0.75f || std::fabs(Two) < 0.75f) continue;
                    Sided = Sided && Shown[static_cast<std::size_t>(Py * Wide + Px)] == (One > 0 && Two > 0 ? 255 : 0);
                }
        }
        const auto Pair = [&](char32_t One, char32_t Other) {
            const auto A = Ink(One), B = Ink(Other);
            for (std::size_t Here = 0; Here < A.size(); Here++)
                Paired = Paired && std::abs(A[Here] + B[Here] - 255) <= 1;
        };
        for (char32_t Each = 0x1FB3C; Each <= 0x1FB51; Each++) Pair(Each, Each + 22);
        for (char32_t Each = 0x1FB68; Each <= 0x1FB6B; Each++) Pair(Each, Each + 4);
        if (!Sided || !Paired) std::printf("  wedges at %d by %d\n", Wide, Tall);
    }
    Count.Check(Sided, "each of the 44 wedges inks its corner's side of the diagonal its name says, and each of the "
                       "4 triangles the part between its side and the middle — away from a diagonal by more than "
                       "the pixel it may share, at every size");
    Count.Check(Paired, "each wedge and its complement, each triangle and its three quarters, share every pixel of "
                        "the cell to within a step of rounding");
}

}  // namespace stand
