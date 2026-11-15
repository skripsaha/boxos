/* font.cpp — a TrueType font, read from its file's bytes. */

#include "typecase/font.h"

#include <algorithm>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "bytes.h"
#include "character_map.h"
#include "glyph_reader.h"

namespace typecase {

namespace {

constexpr std::uint32_t TagOf(std::string_view Name) noexcept
{
    return static_cast<std::uint32_t>(Name[0]) << 24 | static_cast<std::uint32_t>(Name[1]) << 16 |
           static_cast<std::uint32_t>(Name[2]) << 8 | static_cast<std::uint32_t>(Name[3]);
}

constexpr std::uint32_t TrueTypeVersion = 0x00010000;
constexpr std::uint32_t HeadMagic       = 0x5F0F3CF5;

/* OS/2 fsSelection: the line is the typographic ascender, descender and gap. */
constexpr std::uint16_t UseTypoMetrics = 1u << 7;

Bytes View(const std::vector<std::uint8_t> &Data, std::uint32_t Offset, std::uint32_t Length) noexcept
{
    return Bytes(std::span(Data)).Part(Offset, Length);
}

}  // namespace

std::expected<Font, Fault> Font::Read(std::vector<std::uint8_t> Bytes, std::uint32_t Index)
{
    Font Made;
    Made.File = std::move(Bytes);
    if (auto Broken = Made.ReadDirectory(Index)) return std::unexpected(std::move(*Broken));
    if (auto Broken = Made.ReadMeasures()) return std::unexpected(std::move(*Broken));

    const auto Choice = ChooseMap(View(Made.File, Made.Cmap.Offset, Made.Cmap.Length));
    if (!Choice) return std::unexpected(Choice.error());
    Made.Map       = Span{Made.Cmap.Offset + Choice->Offset, Made.Cmap.Length - Choice->Offset};
    Made.MapFormat = Choice->Format;
    Made.SymbolMap = Choice->Symbol;

    Made.FindDamage();
    return Made;
}

/* The table directory — of the one font in the file, or of the one at Index
 * in a collection — and where each table this reader reads lies. A table it
 * does not read may be anything. */
std::optional<Fault> Font::ReadDirectory(std::uint32_t Index)
{
    const Bytes Whole{std::span(File)};
    if (!Whole.Holds(0, 12)) return Fault{"the file is too short to be a font"};

    std::uint32_t Directory = 0;
    if (Whole.U32(0) == TagOf("ttcf")) {
        const std::uint32_t Fonts = Whole.U32(8);
        if (Index >= Fonts)
            return Fault{std::format("the collection holds {} fonts; it has no font {}", Fonts, Index)};
        if (!Whole.Holds(12 + std::size_t{Index} * 4, 4)) return Fault{"the collection's list runs past the file"};
        Directory = Whole.U32(12 + std::size_t{Index} * 4);
        if (!Whole.Holds(Directory, 12)) return Fault{"the collection names a font past the file"};
    } else if (Index != 0) {
        return Fault{std::format("the file holds one font; it has no font {}", Index)};
    }

    const std::uint32_t Version = Whole.U32(Directory);
    if (Version == TagOf("OTTO"))
        return Fault{"its outlines are PostScript curves (CFF), which are not read yet"};
    if (Version != TrueTypeVersion && Version != TagOf("true")) return Fault{"it is not a TrueType font"};

    const std::size_t Tables = Whole.U16(Directory + 4);
    if (!Whole.Holds(Directory + 12, Tables * 16)) return Fault{"its table directory runs past the file"};

    struct Wanted {
        std::string_view Name;
        Span            *Into;
        bool             Needed;
    };
    const Wanted Sought[] = {
        {"head", &Head, true}, {"maxp", &Maxp, true}, {"hhea", &Hhea, true}, {"hmtx", &Hmtx, true},
        {"loca", &Loca, true}, {"glyf", &Glyf, true}, {"cmap", &Cmap, true}, {"OS/2", &Os2, false},
    };
    for (const Wanted &Each : Sought) {
        bool Found = false;
        for (std::size_t Record = Directory + 12; Record < Directory + 12 + Tables * 16 && !Found; Record += 16) {
            if (Whole.U32(Record) != TagOf(Each.Name)) continue;
            const std::uint32_t Offset = Whole.U32(Record + 8), Length = Whole.U32(Record + 12);
            if (!Whole.Holds(Offset, Length))
                return Fault{std::format("{}: the table lies past the end of the file", Each.Name)};
            *Each.Into = Span{Offset, Length};
            Found      = true;
        }
        if (!Found && Each.Needed) return Fault{std::format("the font has no {} table", Each.Name)};
    }
    return std::nullopt;
}

/* What the tables say of the font as a whole: its units, its glyphs, the
 * line, and how loca and hmtx are laid. */
std::optional<Fault> Font::ReadMeasures()
{
    const Bytes HeadOf = View(File, Head.Offset, Head.Length);
    if (!HeadOf.Holds(0, 54)) return Fault{"head: the table is shorter than it is"};
    if (HeadOf.U32(12) != HeadMagic) return Fault{"head: its magic number is wrong; this is no font"};
    Measures.UnitsPerEm = HeadOf.U16(18);
    if (Measures.UnitsPerEm < 16 || Measures.UnitsPerEm > 16384)
        return Fault{std::format("head: {} units to the em; a font has 16 to 16384", Measures.UnitsPerEm)};
    const std::int16_t LocaFormat = HeadOf.I16(50);
    if (LocaFormat != 0 && LocaFormat != 1)
        return Fault{std::format("head: loca's format is {}; there are 0 and 1", LocaFormat)};
    LongOffsets = LocaFormat == 1;

    const Bytes MaxpOf = View(File, Maxp.Offset, Maxp.Length);
    if (!MaxpOf.Holds(0, 6)) return Fault{"maxp: the table is shorter than it is"};
    Glyphs = MaxpOf.U16(4);
    if (Glyphs == 0) return Fault{"maxp: the font has no glyphs"};

    const Bytes HheaOf = View(File, Hhea.Offset, Hhea.Length);
    if (!HheaOf.Holds(0, 36)) return Fault{"hhea: the table is shorter than it is"};
    Measures.Ascender  = HheaOf.I16(4);
    Measures.Descender = HheaOf.I16(6);
    Measures.LineGap   = HheaOf.I16(8);
    Advances           = HheaOf.U16(34);
    if (Advances == 0 || Advances > Glyphs)
        return Fault{std::format("hhea: {} advances for {} glyphs", Advances, Glyphs)};

    if (Hmtx.Length < std::size_t{Advances} * 4)
        return Fault{std::format("hmtx: the table is shorter than its {} advances", Advances)};
    if (Loca.Length < (std::size_t{Glyphs} + 1) * (LongOffsets ? 4 : 2))
        return Fault{std::format("loca: the table is shorter than the places of {} glyphs", Glyphs)};

    const Bytes Os2Of = View(File, Os2.Offset, Os2.Length);
    if (Os2Of.Holds(0, 78) && (Os2Of.U16(62) & UseTypoMetrics)) {
        Measures.Ascender  = Os2Of.I16(68);
        Measures.Descender = Os2Of.I16(70);
        Measures.LineGap   = Os2Of.I16(72);
    }
    if (Os2Of.U16(0) >= 2 && Os2Of.Holds(0, 90)) {
        Measures.XHeight   = Os2Of.I16(86);
        Measures.CapHeight = Os2Of.I16(88);
    }
    return std::nullopt;
}

/* Every glyph read once, now: a font that is damaged is said so when it
 * comes, not glyph by glyph as a screen happens to need them. */
void Font::FindDamage()
{
    std::optional<Fault> First;
    for (std::uint32_t Glyph = 0; Glyph < Glyphs; Glyph++) {
        auto Shape = OutlineOf(static_cast<GlyphId>(Glyph));
        if (Shape) continue;
        Unreadable.push_back(static_cast<GlyphId>(Glyph));
        if (!First) First = std::move(Shape.error());
    }
    if (First)
        Damaged = Fault{std::format("{} of its {} glyphs do not read; the first: {}", Unreadable.size(), Glyphs,
                                    First->Said)};
}

GlyphId Font::Find(char32_t Character) const noexcept
{
    const Bytes Subtable = View(File, Map.Offset, Map.Length);
    const GlyphId         Glyph    = FindInMap(Subtable, MapFormat, Character, Glyphs);
    if (Glyph != 0 || !SymbolMap || Character > 0xFF) return Glyph;
    return FindInMap(Subtable, MapFormat, 0xF000 + Character, Glyphs);
}

std::expected<Outline, Fault> Font::OutlineOf(GlyphId Glyph) const
{
    const GlyphTables Tables{View(File, Loca.Offset, Loca.Length), View(File, Glyf.Offset, Glyf.Length),
                             LongOffsets, Glyphs};
    return ReadOutline(Tables, Glyph);
}

std::uint16_t Font::AdvanceOf(GlyphId Glyph) const noexcept
{
    if (Glyph >= Glyphs) return 0;
    const std::size_t Record = std::min<std::size_t>(Glyph, Advances - 1u) * 4;
    return View(File, Hmtx.Offset, Hmtx.Length).U16(Record);
}

bool Font::Readable(GlyphId Glyph) const noexcept
{
    return Glyph < Glyphs && !std::binary_search(Unreadable.begin(), Unreadable.end(), Glyph);
}

}  // namespace typecase
