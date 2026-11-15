/* character_map.cpp — which glyph draws a character. */

#include "character_map.h"

#include <cstddef>

namespace typecase {

namespace {

/* How wide a repertoire a subtable of Format on (Platform, Encoding) maps,
 * the widest the most; 0 for one this reader does not read — another
 * platform's encoding, a format of byte sequences (2, 8), the deprecated 10,
 * or the variation sequences of 14, which name no glyph of their own. */
int Rank(std::uint16_t Platform, std::uint16_t Encoding, std::uint16_t Format) noexcept
{
    const bool Unicode = (Platform == 0 && Encoding != 5) || (Platform == 3 && (Encoding == 1 || Encoding == 10));
    const bool Symbol  = Platform == 3 && Encoding == 0;
    if (Symbol) return Format == 4 || Format == 6 || Format == 12 ? 1 : 0;
    if (!Unicode) return 0;
    switch (Format) {
    case 12: return 6;
    case 13: return 5;
    case 4:  return 4;
    case 6:  return 3;
    case 0:  return 2;
    default: return 0;
    }
}

/* Whether a subtable of Format holds what it says it holds: its arrays
 * within the table, and its ranges in the ascending order a search needs. */
bool Holds(Bytes Subtable, std::uint16_t Format) noexcept
{
    switch (Format) {
    case 0:
        return Subtable.Holds(0, 6 + 256);
    case 4: {
        if (!Subtable.Holds(0, 14)) return false;
        const std::size_t Segments = Subtable.U16(6) / 2u;
        if (Segments == 0 || Subtable.U16(6) % 2u != 0 || !Subtable.Holds(14, 2 + Segments * 8)) return false;
        std::uint32_t Previous = 0;
        for (std::size_t Index = 0; Index < Segments; Index++) {
            const std::uint16_t End   = Subtable.U16(14 + Index * 2);
            const std::uint16_t Start = Subtable.U16(16 + Segments * 2 + Index * 2);
            if (Start > End || (Index > 0 && End <= Previous)) return false;
            Previous = End;
        }
        return true;
    }
    case 6:
        return Subtable.Holds(0, 10) && Subtable.Holds(10, std::size_t{Subtable.U16(8)} * 2);
    case 12:
    case 13: {
        if (!Subtable.Holds(0, 16)) return false;
        const std::size_t Groups = Subtable.U32(12);
        if (Groups > Subtable.Size() / 12 || !Subtable.Holds(16, Groups * 12)) return false;
        std::uint32_t Previous = 0;
        for (std::size_t Index = 0; Index < Groups; Index++) {
            const std::uint32_t Start = Subtable.U32(16 + Index * 12);
            const std::uint32_t End   = Subtable.U32(20 + Index * 12);
            if (Start > End || End > 0x10FFFF || (Index > 0 && Start <= Previous)) return false;
            Previous = End;
        }
        return true;
    }
    default:
        return false;
    }
}

GlyphId FindFormat4(Bytes Subtable, char32_t Character) noexcept
{
    if (Character > 0xFFFF) return 0;
    const std::size_t Segments = Subtable.U16(6) / 2u;
    const std::size_t Ends = 14, Starts = 16 + Segments * 2, Deltas = Starts + Segments * 2,
                      Ranges = Deltas + Segments * 2;

    std::size_t Low = 0, High = Segments;
    while (Low < High) {
        const std::size_t Middle = (Low + High) / 2;
        if (Subtable.U16(Ends + Middle * 2) < Character) Low = Middle + 1;
        else High = Middle;
    }
    if (Low == Segments) return 0;
    const std::uint16_t Start = Subtable.U16(Starts + Low * 2);
    if (Character < Start) return 0;

    const std::uint16_t Delta = Subtable.U16(Deltas + Low * 2);
    const std::uint16_t Range = Subtable.U16(Ranges + Low * 2);
    if (Range == 0) return static_cast<GlyphId>(Character + Delta);
    const std::uint16_t Glyph = Subtable.U16(Ranges + Low * 2 + Range + (Character - Start) * 2);
    return Glyph == 0 ? 0 : static_cast<GlyphId>(Glyph + Delta);
}

GlyphId FindFormat12(Bytes Subtable, std::uint16_t Format, char32_t Character) noexcept
{
    const std::size_t Groups = Subtable.U32(12);
    std::size_t       Low = 0, High = Groups;
    while (Low < High) {
        const std::size_t Middle = (Low + High) / 2;
        if (Subtable.U32(20 + Middle * 12) < Character) Low = Middle + 1;
        else High = Middle;
    }
    if (Low == Groups) return 0;
    const std::uint32_t Start = Subtable.U32(16 + Low * 12);
    if (Character < Start) return 0;
    const std::uint32_t First = Subtable.U32(24 + Low * 12);
    const std::uint32_t Glyph = Format == 13 ? First : First + (Character - Start);
    return Glyph > 0xFFFF ? 0 : static_cast<GlyphId>(Glyph);
}

}  // namespace

std::expected<MapChoice, Fault> ChooseMap(Bytes Map)
{
    if (!Map.Holds(0, 4)) return std::unexpected(Fault{"cmap: the table is shorter than its header"});
    const std::size_t Records = Map.U16(2);
    if (!Map.Holds(4, Records * 8))
        return std::unexpected(Fault{"cmap: its encoding records run past the table"});

    MapChoice Best;
    int       BestRank = 0;
    for (std::size_t Index = 0; Index < Records; Index++) {
        const std::size_t   Record = 4 + Index * 8;
        const std::uint32_t Offset = Map.U32(Record + 4);
        if (!Map.Holds(Offset, 2)) continue;
        const std::uint16_t Platform = Map.U16(Record), Encoding = Map.U16(Record + 2);
        const std::uint16_t Format = Map.U16(Offset);
        const int           Of     = Rank(Platform, Encoding, Format);
        if (Of <= BestRank || !Holds(Map.Part(Offset, Map.Size() - Offset), Format)) continue;
        Best     = MapChoice{Offset, Format, Platform == 3 && Encoding == 0};
        BestRank = Of;
    }
    if (BestRank == 0)
        return std::unexpected(Fault{"cmap: no map of Unicode characters that reads"});
    return Best;
}

GlyphId FindInMap(Bytes Subtable, std::uint16_t Format, char32_t Character, std::uint16_t Glyphs) noexcept
{
    GlyphId Glyph = 0;
    switch (Format) {
    case 0:
        Glyph = Character < 256 ? Subtable.U8(6 + Character) : 0;
        break;
    case 4:
        Glyph = FindFormat4(Subtable, Character);
        break;
    case 6: {
        const std::uint16_t First = Subtable.U16(6), Count = Subtable.U16(8);
        Glyph = Character >= First && Character - First < Count ? Subtable.U16(10 + (Character - First) * 2) : 0;
        break;
    }
    case 12:
    case 13:
        Glyph = FindFormat12(Subtable, Format, Character);
        break;
    default:
        break;
    }
    return Glyph < Glyphs ? Glyph : 0;
}

}  // namespace typecase
