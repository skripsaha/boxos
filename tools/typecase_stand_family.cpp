/* typecase stand — a family of the stand's own, made here byte by byte:
 * Stand Sans regular, bold and italic, and Stand Extra, with the names a
 * font gives in each encoding, weights old and new, the lines under and
 * through text, and kerning of every kind a font says it in — GPOS pairs one
 * by one and by class, behind an extension, a script of its own, values that
 * move a glyph as well as its advance, a lookup that stops at its first
 * answer, the kern table old and Apple's, a subtable that replaces and ones
 * that must be passed over. Each font held to what it was made to say, the
 * book of fonts to how it chooses among them, the look of a glyph to what
 * bold and leaning make of it; the fonts written for the oracle. */

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "typecase/fonts.h"
#include "typecase/glyph_cache.h"
#include "typecase/glyph_look.h"
#include "typecase_stand.h"
#include "typecase_stand_forge.h"

namespace stand {

namespace {

/* Bytes written big-endian, places to come back to. */
struct Blob {
    ByteVector Bytes;

    std::size_t Here() const { return Bytes.size(); }
    void        U16(std::uint32_t Value) { Bytes.push_back(static_cast<std::uint8_t>(Value >> 8)), Bytes.push_back(static_cast<std::uint8_t>(Value)); }
    void        U32(std::uint32_t Value) { U16(Value >> 16), U16(Value & 0xFFFFu); }
    void        Tag(const char *Name) { Bytes.insert(Bytes.end(), Name, Name + 4); }
    void        At(std::size_t Place, std::uint32_t Value) { PutU16(Bytes, Place, static_cast<std::uint16_t>(Value)); }
    void        Add(const ByteVector &More) { Bytes.insert(Bytes.end(), More.begin(), More.end()); }
};

/* Stand Sans's characters: printable ASCII, a no-break space, a combining
 * acute, the hyphen, the bullet and the ellipsis — each a box, an advance of
 * 500 units but for the narrow and the wide. */
std::vector<char32_t> SansCharacters()
{
    std::vector<char32_t> All;
    for (char32_t C = 0x20; C <= 0x7E; C++) All.push_back(C);
    for (char32_t C : {U' ', U'́', U'‐', U'•', U'…'}) All.push_back(C);
    return All;
}

std::uint16_t SansAdvance(char32_t C)
{
    switch (C) {
    case U' ': case U' ': case U'i': case U'l': case U'j': case U'.': case U',': case U':': case U';':
    case U'!': case U'|': case U'\'':
        return 250;
    case U'm': case U'w': case U'M': case U'W': return 900;
    case U'́': return 0;
    case U'…': return 750;
    case U'-': case U'‐': return 333;
    default: return 500;
    }
}

ByteVector Box(std::int32_t Left, std::int32_t Bottom, std::int32_t Right, std::int32_t Top)
{
    return SimpleGlyph({{{Left, Bottom, true}, {Left, Top, true}, {Right, Top, true}, {Right, Bottom, true}}});
}

ByteVector SansGlyph(char32_t C, std::uint16_t Advance)
{
    if (C == U' ' || C == U' ') return {};
    if (C == U'́') return Box(-200, 650, -100, 750);
    if (C == U'-' || C == U'‐') return Box(40, 250, Advance - 40, 330);
    return Box(50, 0, Advance - 50, 600);
}

/* A name table: each record its platform, encoding, language, name and the
 * text as the platform writes it — UTF-16 big-endian, or Mac Roman bytes. */
struct NameRecord {
    std::uint16_t Platform, Encoding, Language, Id;
    std::u32string Text;
};

ByteVector NameTable(const std::vector<NameRecord> &Records)
{
    Blob Out, Storage;
    Out.U16(0), Out.U16(static_cast<std::uint32_t>(Records.size())), Out.U16(6 + 12 * static_cast<std::uint32_t>(Records.size()));
    for (const NameRecord &Each : Records) {
        Blob Text;
        for (char32_t C : Each.Text) {
            if (Each.Platform == 1) {
                Text.Bytes.push_back(C == U'ä' ? 0x8A : static_cast<std::uint8_t>(C));
            } else if (C > 0xFFFF) {
                Text.U16(0xD800 + ((C - 0x10000) >> 10)), Text.U16(0xDC00 + ((C - 0x10000) & 0x3FF));
            } else {
                Text.U16(C);
            }
        }
        Out.U16(Each.Platform), Out.U16(Each.Encoding), Out.U16(Each.Language), Out.U16(Each.Id);
        Out.U16(static_cast<std::uint32_t>(Text.Here())), Out.U16(static_cast<std::uint32_t>(Storage.Here()));
        Storage.Add(Text.Bytes);
    }
    Out.Add(Storage.Bytes);
    return Out.Bytes;
}

/* OS/2, version 2: its weight, its selection, the lowercase's and capitals'
 * heights, the strike's thickness and place. */
ByteVector Os2Table(std::uint16_t Weight, std::uint16_t Selection)
{
    ByteVector Out(96, 0);
    PutU16(Out, 0, 2), PutU16(Out, 4, Weight), PutU16(Out, 26, 50), PutU16(Out, 28, 300), PutU16(Out, 62, Selection);
    PutU16(Out, 86, 500), PutU16(Out, 88, 700);
    return Out;
}

ByteVector PostTable()
{
    ByteVector Out(32, 0);
    PutU32(Out, 0, 0x00030000), PutU16(Out, 8, static_cast<std::uint16_t>(-100)), PutU16(Out, 10, 50);
    return Out;
}

/* A pair adjustment subtable of format 1: for each first glyph, its seconds
 * and their values, in the formats given. */
using Values = std::vector<std::int16_t>;
ByteVector PairsOneByOne(std::uint16_t Format1, std::uint16_t Format2,
                         const std::map<std::uint16_t, std::map<std::uint16_t, std::pair<Values, Values>>> &Pairs)
{
    Blob Out;
    Out.U16(1);
    const std::size_t CoverageAt = Out.Here();
    Out.U16(0), Out.U16(Format1), Out.U16(Format2), Out.U16(static_cast<std::uint32_t>(Pairs.size()));
    const std::size_t SetsAt = Out.Here();
    for (std::size_t I = 0; I < Pairs.size(); I++) Out.U16(0);
    std::size_t Index = 0;
    for (const auto &[First, Seconds] : Pairs) {
        Out.At(SetsAt + Index++ * 2, static_cast<std::uint32_t>(Out.Here()));
        Out.U16(static_cast<std::uint32_t>(Seconds.size()));
        for (const auto &[Second, Value] : Seconds) {
            Out.U16(Second);
            for (std::int16_t V : Value.first) Out.U16(static_cast<std::uint16_t>(V));
            for (std::int16_t V : Value.second) Out.U16(static_cast<std::uint16_t>(V));
        }
    }
    Out.At(CoverageAt, static_cast<std::uint32_t>(Out.Here()));
    Out.U16(1), Out.U16(static_cast<std::uint32_t>(Pairs.size()));
    for (const auto &Each : Pairs) Out.U16(Each.first);
    return Out.Bytes;
}

/* A pair adjustment subtable of format 2: Firsts of class 1 (by ranges),
 * Seconds of class 1 (by a class array), their pair moved Advance. */
ByteVector PairsByClass(const std::vector<std::uint16_t> &Firsts, const std::vector<std::uint16_t> &Seconds, std::int16_t Advance)
{
    Blob Out;
    Out.U16(2);
    const std::size_t Places = Out.Here();
    Out.U16(0), Out.U16(0x0004), Out.U16(0), Out.U16(0), Out.U16(0), Out.U16(2), Out.U16(2);
    for (int C1 = 0; C1 < 2; C1++)
        for (int C2 = 0; C2 < 2; C2++) Out.U16(static_cast<std::uint16_t>(C1 == 1 && C2 == 1 ? Advance : 0));
    Out.At(Places, static_cast<std::uint32_t>(Out.Here()));
    Out.U16(1), Out.U16(static_cast<std::uint32_t>(Firsts.size()));
    for (std::uint16_t G : Firsts) Out.U16(G);
    Out.At(Places + 6, static_cast<std::uint32_t>(Out.Here()));
    Out.U16(2), Out.U16(static_cast<std::uint32_t>(Firsts.size()));
    for (std::uint16_t G : Firsts) Out.U16(G), Out.U16(G), Out.U16(1);
    std::uint16_t Low = 0xFFFF, High = 0;
    for (std::uint16_t G : Seconds) Low = std::min(Low, G), High = std::max(High, G);
    Out.At(Places + 8, static_cast<std::uint32_t>(Out.Here()));
    Out.U16(1), Out.U16(Low), Out.U16(High - Low + 1u);
    for (std::uint32_t G = Low; G <= High; G++)
        Out.U16(std::find(Seconds.begin(), Seconds.end(), G) != Seconds.end() ? 1 : 0);
    return Out.Bytes;
}

struct Lookup {
    std::uint16_t           Type;
    std::vector<ByteVector> Subtables;
};
struct Feature {
    const char                *Tag;
    std::vector<std::uint16_t> Lookups;
};
struct ScriptOfGpos {
    const char                *Tag;
    std::vector<std::uint16_t> Features;
};

ByteVector Gpos(const std::vector<ScriptOfGpos> &Scripts, const std::vector<Feature> &Features, const std::vector<Lookup> &Lookups)
{
    Blob Out;
    Out.U16(1), Out.U16(0), Out.U16(0), Out.U16(0), Out.U16(0);
    Out.At(4, static_cast<std::uint32_t>(Out.Here()));
    const std::size_t ScriptList = Out.Here();
    Out.U16(static_cast<std::uint32_t>(Scripts.size()));
    for (const ScriptOfGpos &Each : Scripts) Out.Tag(Each.Tag), Out.U16(0);
    for (std::size_t S = 0; S < Scripts.size(); S++) {
        Out.At(ScriptList + 2 + S * 6 + 4, static_cast<std::uint32_t>(Out.Here() - ScriptList));
        Out.U16(4), Out.U16(0);
        Out.U16(0), Out.U16(0xFFFF), Out.U16(static_cast<std::uint32_t>(Scripts[S].Features.size()));
        for (std::uint16_t F : Scripts[S].Features) Out.U16(F);
    }
    Out.At(6, static_cast<std::uint32_t>(Out.Here()));
    const std::size_t FeatureList = Out.Here();
    Out.U16(static_cast<std::uint32_t>(Features.size()));
    for (const Feature &Each : Features) Out.Tag(Each.Tag), Out.U16(0);
    for (std::size_t F = 0; F < Features.size(); F++) {
        Out.At(FeatureList + 2 + F * 6 + 4, static_cast<std::uint32_t>(Out.Here() - FeatureList));
        Out.U16(0), Out.U16(static_cast<std::uint32_t>(Features[F].Lookups.size()));
        for (std::uint16_t L : Features[F].Lookups) Out.U16(L);
    }
    Out.At(8, static_cast<std::uint32_t>(Out.Here()));
    const std::size_t LookupList = Out.Here();
    Out.U16(static_cast<std::uint32_t>(Lookups.size()));
    for (std::size_t L = 0; L < Lookups.size(); L++) Out.U16(0);
    for (std::size_t L = 0; L < Lookups.size(); L++) {
        const std::size_t Start = Out.Here();
        Out.At(LookupList + 2 + L * 2, static_cast<std::uint32_t>(Start - LookupList));
        Out.U16(Lookups[L].Type), Out.U16(0), Out.U16(static_cast<std::uint32_t>(Lookups[L].Subtables.size()));
        const std::size_t Offsets = Out.Here();
        for (std::size_t S = 0; S < Lookups[L].Subtables.size(); S++) Out.U16(0);
        for (std::size_t S = 0; S < Lookups[L].Subtables.size(); S++) {
            Out.At(Offsets + S * 2, static_cast<std::uint32_t>(Out.Here() - Start));
            if (Lookups[L].Type == 9) {
                Out.U16(1), Out.U16(2), Out.U32(8);
            }
            Out.Add(Lookups[L].Subtables[S]);
        }
    }
    return Out.Bytes;
}

/* A kern table: the old format's subtables, or Apple's. */
struct KernPairs {
    std::uint16_t                                         Coverage;
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::int16_t> Pairs;
};

ByteVector KernTable(bool Apple, const std::vector<KernPairs> &Subtables)
{
    Blob Out;
    if (Apple) Out.U32(0x00010000), Out.U32(static_cast<std::uint32_t>(Subtables.size()));
    else Out.U16(0), Out.U16(static_cast<std::uint32_t>(Subtables.size()));
    for (const KernPairs &Each : Subtables) {
        const std::uint32_t Length = (Apple ? 8 : 6) + 8 + 6 * static_cast<std::uint32_t>(Each.Pairs.size());
        if (Apple) Out.U32(Length), Out.U16(Each.Coverage), Out.U16(0);
        else Out.U16(0), Out.U16(Length), Out.U16(Each.Coverage);
        Out.U16(static_cast<std::uint32_t>(Each.Pairs.size())), Out.U16(0), Out.U16(0), Out.U16(0);
        for (const auto &[Pair, Value] : Each.Pairs) Out.U16(Pair.first), Out.U16(Pair.second), Out.U16(static_cast<std::uint16_t>(Value));
    }
    return Out.Bytes;
}

std::map<char32_t, std::uint16_t> SansMap()
{
    std::map<char32_t, std::uint16_t> Map;
    const std::vector<char32_t>       All = SansCharacters();
    for (std::size_t I = 0; I < All.size(); I++) Map[All[I]] = static_cast<std::uint16_t>(I + 1);
    return Map;
}

}  // namespace

std::uint16_t SansGlyphOf(char32_t Character)
{
    const auto Map   = SansMap();
    const auto Found = Map.find(Character);
    return Found == Map.end() ? 0 : Found->second;
}

StandFaces MakeStandFaces()
{
    const std::vector<char32_t> All = SansCharacters();
    const auto                  G   = [](char32_t C) { return SansGlyphOf(C); };
    const auto Sans = [&](std::int16_t Wider) {
        Forge Made;
        Made.Glyphs.push_back(Box(50, 0, 450, 700));
        Made.Advances.push_back(500);
        for (char32_t C : All) {
            const std::uint16_t Advance = SansAdvance(C) ? static_cast<std::uint16_t>(SansAdvance(C) + Wider) : 0;
            Made.Glyphs.push_back(SansGlyph(C, Advance));
            Made.Advances.push_back(Advance);
        }
        Made.Maps = {{3, 1, MapFormat4(SansMap())}};
        return Made;
    };

    StandFaces Faces;
    {
        Forge Made = Sans(0);
        const Lookup First{2, {PairsOneByOne(0x0004, 0, {{G('A'), {{G('V'), {{-100}, {}}}, {G('W'), {{-60}, {}}}}}}),
                               PairsOneByOne(0x0005, 0x0001, {{G('A'), {{G('V'), {{0, -999}, {0}}}, {G('Y'), {{0, -40}, {0}}}}},
                                                               {G('L'), {{G('T'), {{10, -30}, {20}}}}}})}};
        const Lookup ByClass{9, {PairsByClass({G('F'), G('T')}, {G('a'), G('e'), G('o')}, -80)}};
        const Lookup Cyrillic{2, {PairsOneByOne(0x0004, 0, {{G('A'), {{G('V'), {{-200}, {}}}}}})}};
        Made.Tables = {
            {"name", NameTable({{1, 0, 0, 1, U"Wrong Name"}, {3, 1, 0x0409, 1, U"Stand Sans"}, {3, 1, 0x0409, 2, U"Regular"},
                                {3, 1, 0x0407, 1, U"Falscher Name"}})},
            {"OS/2", Os2Table(400, 0x40)},
            {"post", PostTable()},
            {"GPOS", Gpos({{"DFLT", {0}}, {"cyrl", {1, 2}}, {"latn", {0}}},
                          {{"kern", {0, 1}}, {"kern", {2}}, {"liga", {0}}}, {First, ByClass, Cyrillic})},
            {"kern", KernTable(false, {{0x0001, {{{G('A'), G('V')}, -500}}}})},
        };
        Faces.Regular = Made.Build();
    }
    {
        Forge Made = Sans(50);
        Made.Tables = {
            {"name", NameTable({{3, 1, 0x0409, 1, U"Stand Sans Bd"}, {3, 1, 0x0409, 2, U"Regular"},
                                {3, 1, 0x0409, 16, U"Stand Sans"}, {3, 1, 0x0409, 17, U"Bold"}})},
            {"OS/2", Os2Table(7, 0x20)},
            {"kern", KernTable(false, {{0x0001, {{{G('A'), G('V')}, -60}, {{G('T'), G('a')}, -40}}},
                                       {0x0009, {{{G('A'), G('V')}, -70}}},
                                       {0x0005, {{{G('T'), G('a')}, -999}}},
                                       {0x0003, {{{G('T'), G('a')}, -999}}}})},
        };
        Faces.Bold = Made.Build();
    }
    {
        Forge Made = Sans(0);
        Made.Tables = {
            {"name", NameTable({{1, 0, 0, 1, U"Stand Sans"}, {1, 0, 0, 2, U"Itälic"}})},
            {"kern", KernTable(true, {{0x0000, {{{G('A'), G('V')}, -50}}}, {0x8000, {{{G('A'), G('V')}, -999}}}})},
        };
        Faces.Italic = Made.Build();
        PutU16(Faces.Italic, FindTable(Faces.Italic, "head").Offset + 44, 2);
    }
    {
        Forge Made;
        Made.Glyphs   = {Box(50, 0, 450, 700), Box(50, 0, 650, 700), Box(100, -100, 900, 800), Box(100, -100, 900, 800),
                         Box(100, -100, 900, 800), Box(100, -100, 300, 100), Box(50, 0, 550, 700)};
        Made.Advances = {500, 700, 1000, 1000, 1000, 1000, 600};
        Made.Maps     = {{3, 1, MapFormat4({{U'Ж', 1}, {U'中', 2}, {U'国', 3}, {U'人', 4},
                                            {U'。', 5}, {U'A', 6}})}};
        Made.Tables   = {{"name", NameTable({{3, 1, 0x0409, 1, U"Stand Extra"}, {3, 1, 0x0409, 2, U"Book \U0001D53C"}})},
                         {"OS/2", Os2Table(400, 0x40)}};
        Faces.Extra = Made.Build();
    }
    return Faces;
}

typecase::Fonts StandBook()
{
    const StandFaces Faces = MakeStandFaces();
    typecase::Fonts  Book;
    for (const ByteVector *Each : {&Faces.Regular, &Faces.Bold, &Faces.Italic, &Faces.Extra})
        Book.Add(*typecase::Font::Read(*Each));
    return Book;
}

void CheckFamily(Tally &Count, const char *ForgeDir)
{
    const StandFaces Faces = MakeStandFaces();
    const char      *Names[] = {"stand-sans-regular", "stand-sans-bold", "stand-sans-italic", "stand-extra"};
    const ByteVector *Bytes[] = {&Faces.Regular, &Faces.Bold, &Faces.Italic, &Faces.Extra};
    for (int I = 0; I < 4; I++) {
        std::ofstream Out(std::string(ForgeDir) + "/" + Names[I] + ".ttf", std::ios::binary);
        Out.write(reinterpret_cast<const char *>(Bytes[I]->data()), static_cast<std::streamsize>(Bytes[I]->size()));
    }
    const auto Regular = typecase::Font::Read(Faces.Regular), Bold = typecase::Font::Read(Faces.Bold);
    const auto Italic = typecase::Font::Read(Faces.Italic), Extra = typecase::Font::Read(Faces.Extra);
    if (!Regular || !Bold || !Italic || !Extra) {
        Count.Check(false, "the stand's family reads");
        return;
    }
    const auto Said = [](const typecase::Font &F) {
        return F.Style().Family + " / " + F.Style().Name + " / " + std::to_string(F.Style().Weight) + (F.Style().Italic ? " italic" : "");
    };
    Count.Check(Said(*Regular) == "Stand Sans / Regular / 400" && Said(*Bold) == "Stand Sans / Bold / 700" &&
                    Said(*Italic) == "Stand Sans / It\xC3\xA4lic / 400 italic" &&
                    Said(*Extra) == "Stand Extra / Book \xF0\x9D\x94\xBC / 400",
                "faces say their family and name — Windows' in English before Mac Roman and German, the typographic "
                "family before the old, Mac Roman and a surrogate pair decoded — their weight, old 1-9 too, and "
                "italic from head when there is no OS/2: " + Said(*Regular) + "; " + Said(*Bold) + "; " + Said(*Italic) +
                    "; " + Said(*Extra));
    const typecase::FontMetrics &M = Regular->Metrics();
    Count.Check(M.UnderlinePosition == -100 && M.UnderlineThickness == 50 && M.StrikePosition == 300 &&
                    M.StrikeThickness == 50 && Italic->Metrics().UnderlineThickness == 0,
                "the lines under and through text as post and OS/2 say them, none where the font has neither");
    Count.Check(Regular->Serial() != Bold->Serial() && typecase::Font(*Regular).Serial() == Regular->Serial(),
                "every font read has a serial of its own, a copy the same");

    using typecase::PairKerning;
    using typecase::Script;
    const auto K = [](const typecase::Font &F, char32_t A, char32_t B, Script S = Script::Latin) {
        return F.KerningOf(F.Find(A), F.Find(B), S);
    };
    Count.Check(K(*Regular, 'A', 'V') == PairKerning{0, -100, 0, 0} && K(*Regular, 'A', 'W') == PairKerning{0, -60, 0, 0} &&
                    K(*Regular, 'A', 'Y') == PairKerning{0, -40, 0, 0} && K(*Regular, 'L', 'T') == PairKerning{10, -30, 20, 0} &&
                    K(*Regular, 'V', 'A') == PairKerning{},
                "GPOS pairs one by one: a lookup's first subtable that has the pair answers and the next is not asked, "
                "one that has it not passes to the next; values that move the glyph as well as its advance");
    Count.Check(K(*Regular, 'T', 'a') == PairKerning{0, -80, 0, 0} && K(*Regular, 'F', 'o') == PairKerning{0, -80, 0, 0} &&
                    K(*Regular, 'T', 'x') == PairKerning{},
                "GPOS pairs by class, behind an extension lookup; a second of class none kerned none");
    Count.Check(K(*Regular, 'A', 'V', Script::Cyrillic) == PairKerning{0, -200, 0, 0} &&
                    K(*Regular, 'A', 'V', Script::Greek) == PairKerning{0, -100, 0, 0} &&
                    K(*Regular, 'A', 'V', Script::Other) == PairKerning{0, -100, 0, 0},
                "each writing its own kerning — Cyrillic's lookup and not liga's; Greek, which the font names not, "
                "by its default — and the kern table passed over where GPOS kerns");
    Count.Check(K(*Bold, 'A', 'V') == PairKerning{0, -70, 0, 0} && K(*Bold, 'T', 'a') == PairKerning{0, -40, 0, 0},
                "the kern table: subtables added, one that replaces replacing, across-the-line and least ones passed over");
    Count.Check(K(*Italic, 'A', 'V') == PairKerning{0, -50, 0, 0} && Italic->Kerns() && !Extra->Kerns() &&
                    K(*Extra, 'A', 'A') == PairKerning{},
                "Apple's kern table, its vertical subtable passed over; a font that kerns nothing kerns nothing");

    typecase::Fonts Book;
    for (const auto *Each : {&*Regular, &*Bold, &*Italic, &*Extra}) Book.Add(*Each);
    const auto Is = [](const typecase::FaceChoice &Got, const typecase::Font &Want, bool Bold2, bool Slant) {
        return Got.Face && Got.Face->Serial() == Want.Serial() && Got.Bold == Bold2 && Got.Slant == Slant;
    };
    Count.Check(Book.Families() == std::vector<std::string>{"Stand Sans", "Stand Extra"} &&
                    Is(Book.Choose("Stand Sans", 400, false), *Regular, false, false) &&
                    Is(Book.Choose("stand sans", 700, false), *Bold, false, false) &&
                    Is(Book.Choose("Stand Sans", 900, false), *Bold, false, false) &&
                    Is(Book.Choose("Stand Sans", 500, false), *Regular, false, false) &&
                    Is(Book.Choose("Stand Sans", 600, false), *Bold, false, false) &&
                    Is(Book.Choose("Stand Sans", 300, false), *Regular, false, false),
                "faces gathered by family, a family named in either case; the weight nearest as CSS walks the weights");
    Count.Check(Is(Book.Choose("Stand Sans", 400, true), *Italic, false, false) &&
                    Is(Book.Choose("Stand Sans", 700, true), *Italic, true, false) &&
                    Is(Book.Choose("Stand Extra", 700, true), *Extra, true, true) &&
                    Is(Book.Choose("Nowhere, 'Stand Extra'", 400, false), *Extra, false, false) &&
                    Is(Book.Choose("", 400, false), *Regular, false, false),
                "a face that leans before one of the nearer weight; bold and leaning made where the family has no "
                "face that is; the first family named that is here; the default, the first family added");
    Book.SetDefault("Stand Extra");
    const bool Defaulted = Is(Book.Choose("", 400, false), *Extra, false, false);
    Book.SetDefault("");
    Count.Check(Defaulted && Is(Book.ChooseFor(U'Ж', "Stand Sans", 400, false), *Extra, false, false) &&
                    !Book.ChooseFor(U'\U0001F600', "Stand Sans", 400, false).Face &&
                    Book.Choices("Stand Sans", 400, false).size() == 2,
                "a default set; a character the family has not from the next that has it; none from a book that has "
                "it nowhere");

    /* Bold and leaning, as drawn: H is a box 400 units wide and 600 tall. */
    typecase::Rasterizer Pen;
    const typecase::GlyphId H = Regular->Find(U'H');
    const auto Ink = [](const typecase::GlyphImage &Image) {
        double Sum = 0;
        for (std::uint8_t C : Image.Coverage) Sum += C / 255.0;
        return Sum;
    };
    const auto Plain  = typecase::DrawGlyph(*Regular, H, typecase::GlyphLook{40}, Pen);
    const auto Heavy  = typecase::DrawGlyph(*Regular, H, typecase::GlyphLook{40, 0, true, false}, Pen);
    const auto Leaned = typecase::DrawGlyph(*Regular, H, typecase::GlyphLook{40, 0, false, true}, Pen);
    const auto Moved  = typecase::DrawGlyph(*Regular, H, typecase::GlyphLook{40, 2, false, false}, Pen);
    const float Growth = typecase::BoldGrowth(40);
    char        Line[300];
    std::snprintf(Line, sizeof Line,
                  "bold made of a regular face: H grown %.2f px a side and kept at its left edge and foot — ink %.1f of "
                  "%.1f, advance %.2f of %.2f",
                  Growth / 2, Ink(Heavy), (16 + Growth) * (24 + Growth), Heavy.Advance, Plain.Advance + Growth);
    Count.Check(std::fabs(Ink(Plain) - 16 * 24) < 0.5 && std::fabs(Ink(Heavy) - (16 + Growth) * (24 + Growth)) < 0.5 &&
                    std::fabs(Heavy.Advance - Plain.Advance - Growth) < 1e-4 && Heavy.Left == Plain.Left &&
                    Heavy.Top + Heavy.Height == Plain.Top + Plain.Height,
                Line);
    std::int32_t TopLeft = Leaned.Width, FootLeft = Leaned.Width;
    for (std::int32_t X = 0; X < Leaned.Width; X++) {
        if (Leaned.Coverage[static_cast<std::size_t>(X)] > 127 && TopLeft == Leaned.Width) TopLeft = X;
        if (Leaned.Coverage[static_cast<std::size_t>((Leaned.Height - 1) * Leaned.Width + X)] > 127 && FootLeft == Leaned.Width)
            FootLeft = X;
    }
    std::snprintf(Line, sizeof Line, "leaning made of an upright face: the same ink (%.1f), its top %d px right of its foot",
                  Ink(Leaned), (Leaned.Left + TopLeft) - (Leaned.Left + FootLeft));
    Count.Check(std::fabs(Ink(Leaned) - Ink(Plain)) < 0.5 && std::abs(TopLeft - FootLeft - 5) <= 1 &&
                    Leaned.Advance == Plain.Advance,
                Line);
    double Before = 0, After = 0;
    for (std::int32_t Y = 0; Y < Plain.Height; Y++)
        for (std::int32_t X = 0; X < Plain.Width; X++)
            Before += (Plain.Left + X + 0.5) * Plain.Coverage[static_cast<std::size_t>(Y * Plain.Width + X)];
    for (std::int32_t Y = 0; Y < Moved.Height; Y++)
        for (std::int32_t X = 0; X < Moved.Width; X++)
            After += (Moved.Left + X + 0.5) * Moved.Coverage[static_cast<std::size_t>(Y * Moved.Width + X)];
    Count.Check(std::fabs((After - Before) / (Ink(Plain) * 255) - 0.5) < 0.01 && std::fabs(Ink(Moved) - Ink(Plain)) < 0.5,
                "a pen two quarters of a pixel right draws the glyph half a pixel right, its ink kept");

    typecase::GlyphCache Kept(1 << 20);
    const auto           First  = Kept.Get(*Regular, H, typecase::GlyphLook{40}).Coverage;
    const auto           Heavy2 = Kept.Get(*Regular, H, typecase::GlyphLook{40, 0, true, false}).Coverage;
    const auto           Again  = Kept.Get(*Regular, H, typecase::GlyphLook{40}).Coverage;
    const auto           Code   = Kept.GetMissing(U'\U0001F600', typecase::Size{20, 17, 20, 16});
    Count.Check(First == Plain.Coverage && Heavy2 == Heavy.Coverage && Again == First && Code.Width == 17 &&
                    Code.Height == 20 && Code.Top == -16,
                "the cache keeps a font's glyphs by their look apart, and a missing character's box");
}

}  // namespace stand
