/* kerning.cpp — how two glyphs side by side are set: GPOS's pair
 * adjustments under its 'kern' feature, or, in a font whose GPOS kerns
 * nothing, the kern table's pairs. Nothing in either is trusted: every
 * offset is read through Bytes, so a table that lies kerns nothing rather
 * than reads past its end. */

#include "typecase/font.h"

#include <algorithm>
#include <bit>
#include <span>
#include <string_view>

#include "bytes.h"

namespace typecase {

namespace {

constexpr std::uint32_t TagOf(std::string_view Name) noexcept
{
    return static_cast<std::uint32_t>(Name[0]) << 24 | static_cast<std::uint32_t>(Name[1]) << 16 |
           static_cast<std::uint32_t>(Name[2]) << 8 | static_cast<std::uint32_t>(Name[3]);
}

/* GPOS lookup types: a pair's adjustment, and the extension that holds a
 * subtable of another type farther away. */
constexpr std::uint16_t PairAdjustment = 2, Extension = 9;

/* A value record's fields, two bytes each, in this order. */
constexpr std::uint16_t XPlacement = 0x0001, YPlacement = 0x0002, XAdvance = 0x0004;

/* The kern table's coverage bits: the old format's, and Apple's. */
constexpr std::uint16_t KernHorizontal = 0x01, KernMinimum = 0x02, KernAcross = 0x04, KernReplaces = 0x08;
constexpr std::uint16_t AppleVertical = 0x8000, AppleAcross = 0x4000, AppleVariation = 0x2000;

/* What lies from At to the end of Of. */
Bytes From(Bytes Of, std::size_t At) noexcept { return At <= Of.Size() ? Of.Part(At, Of.Size() - At) : Bytes(); }

std::size_t ValueSize(std::uint16_t Format) noexcept
{
    return 2u * static_cast<std::size_t>(std::popcount(static_cast<unsigned>(Format & 0xFFu)));
}

struct Moves {
    std::int32_t Shift   = 0;
    std::int32_t Advance = 0;
};

/* The horizontal shift and advance of the value record at At. */
Moves ReadValue(Bytes Table, std::size_t At, std::uint16_t Format) noexcept
{
    Moves Got;
    if (Format & XPlacement) Got.Shift = Table.I16(At), At += 2;
    if (Format & YPlacement) At += 2;
    if (Format & XAdvance) Got.Advance = Table.I16(At);
    return Got;
}

/* The glyph's place in a coverage table; -1 when it is not covered. */
std::int32_t CoverageIndex(Bytes Coverage, GlyphId Glyph) noexcept
{
    const std::uint16_t Format = Coverage.U16(0);
    const std::size_t   Count  = Coverage.U16(2);
    std::size_t         Low = 0, High = Count;
    if (Format == 1) {
        while (Low < High) {
            const std::size_t Middle = (Low + High) / 2;
            const GlyphId     Here   = Coverage.U16(4 + Middle * 2);
            if (Here == Glyph) return static_cast<std::int32_t>(Middle);
            if (Here < Glyph) Low = Middle + 1;
            else High = Middle;
        }
    } else if (Format == 2) {
        while (Low < High) {
            const std::size_t Middle = (Low + High) / 2;
            if (Coverage.U16(6 + Middle * 6) < Glyph) Low = Middle + 1;
            else High = Middle;
        }
        if (Low < Count && Coverage.U16(4 + Low * 6) <= Glyph)
            return Coverage.U16(8 + Low * 6) + (Glyph - Coverage.U16(4 + Low * 6));
    }
    return -1;
}

/* The glyph's class in a class definition; 0 for one it does not name. */
std::uint16_t ClassOf(Bytes Classes, GlyphId Glyph) noexcept
{
    const std::uint16_t Format = Classes.U16(0);
    if (Format == 1) {
        const GlyphId First = Classes.U16(2);
        return Glyph >= First && Glyph - First < Classes.U16(4) ? Classes.U16(6 + (Glyph - First) * 2u) : 0;
    }
    if (Format == 2) {
        const std::size_t Count = Classes.U16(2);
        std::size_t       Low = 0, High = Count;
        while (Low < High) {
            const std::size_t Middle = (Low + High) / 2;
            if (Classes.U16(6 + Middle * 6) < Glyph) Low = Middle + 1;
            else High = Middle;
        }
        if (Low < Count && Classes.U16(4 + Low * 6) <= Glyph) return Classes.U16(8 + Low * 6);
    }
    return 0;
}

void Add(PairKerning &Into, Moves First, Moves Second) noexcept
{
    Into.FirstShift += First.Shift;
    Into.FirstAdvance += First.Advance;
    Into.SecondShift += Second.Shift;
    Into.SecondAdvance += Second.Advance;
}

/* A pair adjustment subtable's say on the pair, added: whether it has one —
 * after which no other subtable of its lookup is asked. */
bool ApplyPair(Bytes Subtable, GlyphId First, GlyphId Second, PairKerning &Into) noexcept
{
    const std::int32_t Covered = CoverageIndex(From(Subtable, Subtable.U16(2)), First);
    if (Covered < 0) return false;
    const std::uint16_t Format1 = Subtable.U16(4), Format2 = Subtable.U16(6);
    const std::size_t   Size1 = ValueSize(Format1), Size2 = ValueSize(Format2);
    switch (Subtable.U16(0)) {
    case 1: {
        if (static_cast<std::size_t>(Covered) >= Subtable.U16(8)) return false;
        const Bytes       Set    = From(Subtable, Subtable.U16(10 + static_cast<std::size_t>(Covered) * 2));
        const std::size_t Record = 2 + Size1 + Size2;
        std::size_t       Low = 0, High = Set.U16(0);
        while (Low < High) {
            const std::size_t Middle = (Low + High) / 2, At = 2 + Middle * Record;
            const GlyphId     Here   = Set.U16(At);
            if (Here == Second) {
                Add(Into, ReadValue(Set, At + 2, Format1), ReadValue(Set, At + 2 + Size1, Format2));
                return true;
            }
            if (Here < Second) Low = Middle + 1;
            else High = Middle;
        }
        return false;
    }
    case 2: {
        const std::uint16_t Class1 = ClassOf(From(Subtable, Subtable.U16(8)), First);
        const std::uint16_t Class2 = ClassOf(From(Subtable, Subtable.U16(10)), Second);
        const std::size_t   Count1 = Subtable.U16(12), Count2 = Subtable.U16(14);
        if (Class1 >= Count1 || Class2 >= Count2) return false;
        const std::size_t At = 16 + (Class1 * Count2 + Class2) * (Size1 + Size2);
        Add(Into, ReadValue(Subtable, At, Format1), ReadValue(Subtable, At + Size1, Format2));
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

void Font::ReadKerning()
{
    const Bytes G = Bytes(std::span(File)).Part(Gpos.Offset, Gpos.Length);
    if (G.Holds(0, 10) && G.U16(0) == 1) {
        const std::size_t ScriptList = G.U16(4), FeatureList = G.U16(6), LookupList = G.U16(8);
        const Bytes       Features = From(G, FeatureList), Lookups = From(G, LookupList);

        /* GPOS's lookup index, to its place among the kerning lookups kept. */
        std::vector<std::int32_t> Kept(Lookups.U16(0), -1);
        const auto Keep = [&](std::size_t Index) -> std::int32_t {
            if (Index >= Kept.size()) return -1;
            if (Kept[Index] >= 0) return Kept[Index];
            const std::size_t   Lookup = LookupList + Lookups.U16(2 + Index * 2);
            const Bytes         Of     = From(G, Lookup);
            const std::uint16_t Type   = Of.U16(0);
            std::vector<std::uint32_t> Subtables;
            for (std::size_t Sub = 0; Sub < Of.U16(4); Sub++) {
                std::size_t At = Lookup + Of.U16(6 + Sub * 2);
                if (Type == Extension) {
                    const Bytes Far = From(G, At);
                    if (Far.U16(0) != 1 || Far.U16(2) != PairAdjustment) continue;
                    At += Far.U32(4);
                } else if (Type != PairAdjustment) {
                    break;
                }
                if (At < G.Size()) Subtables.push_back(static_cast<std::uint32_t>(Gpos.Offset + At));
            }
            if (Subtables.empty()) return -1;
            Kerning.Lookups.push_back(std::move(Subtables));
            return Kept[Index] = static_cast<std::int32_t>(Kerning.Lookups.size() - 1);
        };

        const Bytes Scripts = From(G, ScriptList);
        for (std::size_t Record = 0; Record < Scripts.U16(0); Record++) {
            const std::size_t At     = 2 + Record * 6;
            const Bytes       Script = From(Scripts, Scripts.U16(At + 4));
            std::size_t       System = Script.U16(0);
            if (System == 0 && Script.U16(2) > 0) System = Script.U16(6);
            if (System == 0) continue;
            const Bytes                LangSys = From(Script, System);
            std::vector<std::uint16_t> Indices;
            const auto                 Take = [&](std::size_t Feature) {
                if (Feature >= Features.U16(0) || Features.U32(2 + Feature * 6) != TagOf("kern")) return;
                const Bytes Of = From(Features, Features.U16(6 + Feature * 6));
                for (std::size_t Each = 0; Each < Of.U16(2); Each++) {
                    const std::size_t Lookup = Of.U16(4 + Each * 2);
                    if (Lookup < Kept.size()) Indices.push_back(static_cast<std::uint16_t>(Lookup));
                }
            };
            if (LangSys.U16(2) != 0xFFFF) Take(LangSys.U16(2));
            for (std::size_t Each = 0; Each < LangSys.U16(4); Each++) Take(LangSys.U16(6 + Each * 2));
            std::sort(Indices.begin(), Indices.end());
            Indices.erase(std::unique(Indices.begin(), Indices.end()), Indices.end());

            KerningPlan::ForScript For{Scripts.U32(At), {}};
            for (std::uint16_t Index : Indices)
                if (const std::int32_t Place = Keep(Index); Place >= 0)
                    For.Lookups.push_back(static_cast<std::uint16_t>(Place));
            if (!For.Lookups.empty()) Kerning.Scripts.push_back(std::move(For));
        }
    }
    if (!Kerning.Scripts.empty()) return;
    Kerning.Lookups.clear();

    const Bytes K = Bytes(std::span(File)).Part(Kern.Offset, Kern.Length);
    const bool  Apple = K.U32(0) == 0x00010000;
    std::size_t Tables = Apple ? K.U32(4) : K.U16(0) == 0 ? K.U16(2) : 0;
    std::size_t At     = Apple ? 8 : 4;
    for (; Tables > 0 && K.Holds(At, Apple ? 8 : 6); Tables--) {
        const std::size_t   Length   = Apple ? K.U32(At) : K.U16(At + 2);
        const std::uint16_t Coverage = Apple ? K.U16(At + 4) : K.U16(At + 4);
        const std::size_t   Body     = At + (Apple ? 8 : 6);
        const bool          Format0  = Apple ? (Coverage & 0xFF) == 0 : (Coverage >> 8) == 0;
        const bool          Wanted   = Apple ? !(Coverage & (AppleVertical | AppleAcross | AppleVariation))
                                             : (Coverage & KernHorizontal) && !(Coverage & (KernMinimum | KernAcross));
        if (Format0 && Wanted && K.Holds(Body, 8)) {
            const std::size_t Room  = (K.Size() - Body - 8) / 6;
            const std::size_t Count = std::min<std::size_t>(K.U16(Body), Room);
            Kerning.Table.push_back(KerningPlan::Pairs{static_cast<std::uint32_t>(Kern.Offset + Body + 8),
                                                       static_cast<std::uint32_t>(Count),
                                                       !Apple && (Coverage & KernReplaces) != 0});
        }
        /* A subtable of more than 10922 pairs is longer than its 16-bit
         * length can say: the next one lies past its pairs. */
        const std::size_t Pairs = Format0 ? K.U16(Body) * std::size_t{6} + 8 : 0;
        At = Body + std::max(Pairs, Length > (Apple ? 8u : 6u) ? Length - (Apple ? 8u : 6u) : 0u);
        if (Length == 0 && Pairs == 0) break;
    }
}

const Font::KerningPlan::ForScript *Font::LookupsFor(Script Of) const noexcept
{
    const std::uint32_t Wanted = Of == Script::Latin      ? TagOf("latn")
                                 : Of == Script::Greek    ? TagOf("grek")
                                 : Of == Script::Cyrillic ? TagOf("cyrl")
                                                          : TagOf("DFLT");
    for (std::uint32_t Tag : {Wanted, TagOf("DFLT"), TagOf("dflt"), TagOf("latn")})
        for (const KerningPlan::ForScript &For : Kerning.Scripts)
            if (For.Tag == Tag) return &For;
    return nullptr;
}

PairKerning Font::KerningOf(GlyphId First, GlyphId Second, Script Of) const noexcept
{
    PairKerning Got;
    const Bytes Whole{std::span(File)};
    if (!Kerning.Scripts.empty()) {
        const KerningPlan::ForScript *For = LookupsFor(Of);
        if (!For) return Got;
        const std::size_t End = std::size_t{Gpos.Offset} + Gpos.Length;
        for (std::uint16_t Lookup : For->Lookups)
            for (std::uint32_t Subtable : Kerning.Lookups[Lookup])
                if (ApplyPair(Whole.Part(Subtable, End - Subtable), First, Second, Got)) break;
        return Got;
    }
    const std::uint32_t Key = static_cast<std::uint32_t>(First) << 16 | Second;
    for (const KerningPlan::Pairs &Pairs : Kerning.Table) {
        std::size_t Low = 0, High = Pairs.Count;
        while (Low < High) {
            const std::size_t   Middle = (Low + High) / 2, At = Pairs.Offset + Middle * 6;
            const std::uint32_t Here   = Whole.U32(At);
            if (Here == Key) {
                const std::int32_t Value = Whole.I16(At + 4);
                Got.FirstAdvance         = Pairs.Replaces ? Value : Got.FirstAdvance + Value;
                break;
            }
            if (Here < Key) Low = Middle + 1;
            else High = Middle;
        }
    }
    return Got;
}

bool Font::Kerns() const noexcept { return !Kerning.Scripts.empty() || !Kerning.Table.empty(); }

}  // namespace typecase
