/* typecase stand — the stand's own fonts read: every character map format,
 * the components of a composite put in place each way glyf has, nesting to
 * the limit and past it, cycles, a collection. Each read is held to what
 * the font was made to say; and the fonts that read whole are written out
 * for tools/typecase_oracle.py to say as well (typecase_stand.sh). */

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "typecase/font.h"
#include "typecase_stand.h"
#include "typecase_stand_forge.h"

namespace stand {

namespace {

enum : std::uint16_t {
    Offsets      = 0x0002,
    OneScale     = 0x0008,
    TwoScales    = 0x0040,
    TwoByTwo     = 0x0080,
    ScaledOffset = 0x0800,
};

constexpr std::int16_t Half = 8192, OneAndHalf = 24576, One = 16384;

const ForgeContour SquareContour{{0, 0, true}, {100, 0, true}, {100, 100, true}, {0, 100, true}};

/* The glyphs every forged font begins with: none, a square, a contour all
 * off the curve, one that begins off it and ends on it. */
std::vector<ByteVector> BaseGlyphs()
{
    return {
        {},
        SimpleGlyph({SquareContour}),
        SimpleGlyph({{{100, 0, false}, {200, 100, false}, {100, 200, false}, {0, 100, false}}}),
        SimpleGlyph({{{0, 100, false}, {100, 100, true}, {100, 0, true}, {0, 0, true}}}),
    };
}

ByteVector WithMaps(std::vector<ForgeMap> Maps)
{
    Forge Made;
    Made.Glyphs = BaseGlyphs();
    Made.Maps   = std::move(Maps);
    return Made.Build();
}

/* The outline as words: a step's letter and its points, to compare with
 * what a forged glyph was made to be. */
std::string Spoken(const typecase::Outline &Shape)
{
    std::string Said;
    std::size_t At = 0;
    for (typecase::Step Each : Shape.Steps) {
        Said += Each == typecase::Step::Move ? "M" : Each == typecase::Step::Line ? "L" : "Q";
        for (int Index = 0; Index < (Each == typecase::Step::Quad ? 2 : 1); Index++, At++) {
            char Point[48];
            std::snprintf(Point, sizeof Point, " %g,%g", static_cast<double>(Shape.Points[At].X),
                          static_cast<double>(Shape.Points[At].Y));
            Said += Point;
        }
        Said += ' ';
    }
    return Said;
}

std::string SpokenGlyph(const typecase::Font &Font, typecase::GlyphId Glyph)
{
    const auto Shape = Font.OutlineOf(Glyph);
    return Shape ? Spoken(*Shape) : "fault: " + Shape.error().Said;
}

void Write(const char *Dir, const std::string &Name, const ByteVector &Bytes)
{
    std::ofstream Out(std::string(Dir) + "/" + Name, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
}

void CheckMaps(Tally &Count, const char *Dir)
{
    const std::map<char32_t, std::uint16_t> Latin{{U'A', 1}, {U'B', 2}, {U'C', 3}};
    std::map<char32_t, std::uint16_t>       Wide = Latin;
    Wide[U'\U0001F600'] = 2, Wide[U'\U0010FFFF'] = 1;

    const ByteVector Full = WithMaps({{3, 1, MapFormat4(Latin)}, {3, 10, MapFormat12(Wide)}});
    auto F = typecase::Font::Read(Full);
    Count.Check(F && F->Find(U'\U0001F600') == 2 && F->Find(U'\U0010FFFF') == 1 && F->Find(U'B') == 2 &&
                    F->Find(U'D') == 0,
                "format 12 beside format 4: the wider read — U+1F600 and U+10FFFF found");
    Write(Dir, "maps_12.ttf", Full);

    const ByteVector Ranged = WithMaps({{3, 1, MapFormat4({{U'A', 1}, {U'B', 2}, {U'C', 1}, {U'€', 3}})}});
    F = typecase::Font::Read(Ranged);
    Count.Check(F && F->Find(U'A') == 1 && F->Find(U'C') == 1 && F->Find(U'€') == 3 && F->Find(U'D') == 0 &&
                    F->Find(U'\U0001F600') == 0,
                "format 4 by a range into its glyph array and by a delta");
    Write(Dir, "maps_4.ttf", Ranged);

    const ByteVector Six = WithMaps({{3, 1, MapFormat6(U'a', {1, 2, 3})}});
    F = typecase::Font::Read(Six);
    Count.Check(F && F->Find(U'a') == 1 && F->Find(U'c') == 3 && F->Find(U'd') == 0 && F->Find(U'`') == 0,
                "format 6: a run of characters from its first");
    Write(Dir, "maps_6.ttf", Six);

    const ByteVector Zero = WithMaps({{0, 3, MapFormat0({{U'x', 3}})}});
    F = typecase::Font::Read(Zero);
    Count.Check(F && F->Find(U'x') == 3 && F->Find(U'y') == 0 && F->Find(U'Ÿ') == 0, "format 0: a byte's worth");
    Write(Dir, "maps_0.ttf", Zero);

    std::map<char32_t, std::uint16_t> Same;
    for (char32_t Character = 0x4E00; Character <= 0x4E10; Character++) Same[Character] = 3;
    const ByteVector Thirteen = WithMaps({{0, 6, MapFormat12(Same, true)}});
    F = typecase::Font::Read(Thirteen);
    Count.Check(F && F->Find(U'一') == 3 && F->Find(U'丐') == 3 && F->Find(U'丑') == 0,
                "format 13: many characters to one glyph");
    Write(Dir, "maps_13.ttf", Thirteen);

    const ByteVector Symbol = WithMaps({{3, 0, MapFormat4({{U'', 1}})}});
    F = typecase::Font::Read(Symbol);
    Count.Check(F && F->Find(U'A') == 1 && F->Find(U'') == 1 && F->Find(U'B') == 0,
                "a symbol font's map: its characters at U+F000 up, found by their byte too");
    Write(Dir, "maps_symbol.ttf", Symbol);

    ByteVector Disordered = MapFormat12(Wide);
    PutU32(Disordered, 16 + 12, 0);
    F = typecase::Font::Read(WithMaps({{3, 10, Disordered}, {3, 1, MapFormat4(Latin)}}));
    Count.Check(F && F->Find(U'B') == 2 && F->Find(U'\U0001F600') == 0,
                "a format 12 map out of order is passed over for the format 4 beside it");

    F = typecase::Font::Read(WithMaps({{1, 0, MapFormat0({{U'x', 3}})}}));
    Count.Check(!F && F.error().Said == "cmap: no map of Unicode characters that reads",
                "only a Macintosh Roman map: no map of Unicode, said");

    const ByteVector Both = Collection({Ranged, Full});
    const auto       Second = typecase::Font::Read(Both, 1), Third = typecase::Font::Read(Both, 2);
    const auto       Lone   = typecase::Font::Read(Ranged, 1);
    Count.Check(Second && Second->Find(U'\U0001F600') == 2 && typecase::Font::Read(Both, 0)->Find(U'\U0001F600') == 0 &&
                    !Third && !Lone,
                "a collection of two: each font by its index, none past them; a lone font has only font 0");
    Write(Dir, "collection.ttc", Both);
}

void CheckComposites(Tally &Count, const char *Dir)
{
    Forge Made;
    Made.Glyphs = BaseGlyphs();
    Made.Maps   = {{3, 1, MapFormat4({{U'A', 1}})}};
    const auto Add = [&](std::vector<ForgeComponent> Parts) { Made.Glyphs.push_back(CompositeGlyph(Parts)); };
    Add({{1, Offsets, 50, 20, {}}});                                       /* 4 */
    Add({{1, Offsets | OneScale, 10, 10, {Half}}});                        /* 5 */
    Add({{1, Offsets | TwoScales, 0, 0, {OneAndHalf, Half}}});             /* 6 */
    Add({{1, Offsets | TwoByTwo, 0, 0, {0, One, -One, 0}}});               /* 7 */
    Add({{1, Offsets, 0, 0, {}}, {1, 0, 2, 0, {}}});                       /* 8: pinned */
    Add({{1, Offsets | OneScale | ScaledOffset, 100, 40, {Half}}});        /* 9 */
    Add({{1, Offsets, 1000, -1000, {}}, {2, Offsets, -300, 5, {}}});       /* 10: words */
    for (std::uint16_t Glyph = 11; Glyph <= 26; Glyph++)                   /* 11..26: nesting to the limit */
        Add({{static_cast<std::uint16_t>(Glyph == 11 ? 1 : Glyph - 1), Offsets, 1, 0, {}}});
    Write(Dir, "composites.ttf", Made.Build());
    Add({{26, Offsets, 1, 0, {}}});                                        /* 27: past it */
    Add({{29, Offsets, 0, 0, {}}});                                        /* 28: a cycle */
    Add({{28, Offsets, 0, 0, {}}});                                        /* 29 */
    Add({{999, Offsets, 0, 0, {}}});                                       /* 30: past the glyphs */
    Add({{1, Offsets, 0, 0, {}}, {1, 0, 9, 0, {}}});                      /* 31: a point it lacks */
    const auto Read = typecase::Font::Read(Made.Build());
    if (!Read) return Count.Check(false, "the composites font reads: " + Read.error().Said);
    const typecase::Font &F = *Read;

    Count.Check(SpokenGlyph(F, 2) == "M 50,50 Q 100,0 150,50 Q 200,100 150,150 Q 100,200 50,150 Q 0,100 50,50 ",
                "a contour all off the curve begins between its last and first points, every point between two "
                "controls made");
    Count.Check(SpokenGlyph(F, 3) == "M 0,0 Q 0,100 100,100 L 100,0 ",
                "a contour that begins off the curve and ends on it begins at its last point");
    Count.Check(SpokenGlyph(F, 4) == "M 50,20 L 150,20 L 150,120 L 50,120 ", "a component moved by its offset");
    Count.Check(SpokenGlyph(F, 5) == "M 10,10 L 60,10 L 60,60 L 10,60 ", "a component scaled by a half, then moved");
    Count.Check(SpokenGlyph(F, 6) == "M 0,0 L 150,0 L 150,50 L 0,50 ", "a component scaled apart in x and y");
    Count.Check(SpokenGlyph(F, 7) == "M 0,0 L 0,100 L -100,100 L -100,0 ", "a component turned a quarter by its 2x2");
    Count.Check(SpokenGlyph(F, 8) == "M 0,0 L 100,0 L 100,100 L 0,100 M 100,100 L 200,100 L 200,200 L 100,200 ",
                "a component pinned by its point 0 to the point 2 gathered before it");
    Count.Check(SpokenGlyph(F, 9) == "M 50,20 L 100,20 L 100,70 L 50,70 ",
                "an offset scaled with its component, as its flag asks");
    Count.Check(SpokenGlyph(F, 10).starts_with("M 1000,-1000 L 1100,-1000 "), "offsets past a byte, written in words");
    Count.Check(F.Readable(26) && SpokenGlyph(F, 26) == "M 16,0 L 116,0 L 116,100 L 16,100 ",
                "sixteen composites nested above a simple glyph read, each moving it once");
    Count.Check(!F.Readable(27) && SpokenGlyph(F, 27).find("nests components deeper than 16") != std::string::npos,
                "seventeen do not: " + SpokenGlyph(F, 27));
    Count.Check(!F.Readable(28) && !F.Readable(29) && SpokenGlyph(F, 28).find("contains itself") != std::string::npos,
                "a cycle of two composites: neither reads — " + SpokenGlyph(F, 28));
    Count.Check(!F.Readable(30) && SpokenGlyph(F, 30).find("is past the font's") != std::string::npos,
                "a component past the font's glyphs — " + SpokenGlyph(F, 30));
    Count.Check(!F.Readable(31) && SpokenGlyph(F, 31).find("pins a component") != std::string::npos,
                "a component pinned by a point there is not — " + SpokenGlyph(F, 31));
    Count.Check(F.Damage() && F.Damage()->Said.starts_with("5 of its 32 glyphs do not read; the first: glyf: glyph 27"),
                "the damage said once: " + (F.Damage() ? F.Damage()->Said : std::string("none")));
}

}  // namespace

void CheckForged(Tally &Count, const char *ForgeDir)
{
    CheckMaps(Count, ForgeDir);
    CheckComposites(Count, ForgeDir);
}

}  // namespace stand
