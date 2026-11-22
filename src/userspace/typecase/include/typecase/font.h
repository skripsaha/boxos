#ifndef TYPECASE_FONT_H
#define TYPECASE_FONT_H

/* A TrueType font, read from its file's bytes: which glyph draws a
 * character, the glyph's outline, how far it moves the pen, how a pair of
 * glyphs is kerned, the measures of the line, and what the face is within
 * its family — its family's name, its weight, whether it is italic.
 *
 * Read takes the bytes whole and keeps them; it never asks where they came
 * from. Nothing in them is trusted: a table that lies past the end, a glyph
 * whose points run off its record, a component that contains itself — a
 * font that cannot be read is a Fault that says what and where, never a
 * read out of bounds. A font whose tables hold but some of whose glyphs do
 * not is read, and those glyphs are named once (Damage) and drawn by no one
 * (Readable): whoever asks next in a chain draws them.
 *
 * Outlines of PostScript curves (CFF, an .otf of 'OTTO') are not read yet;
 * such a font is a Fault that says so. Hinting programs (fpgm, prep, a
 * glyph's instructions) are passed over: glyphs are drawn as designed. */

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "typecase/outline.h"
#include "typecase/script.h"

namespace typecase {

using GlyphId = std::uint16_t;

/* Why a font, or one of its glyphs, cannot be read: a sentence for a
 * person, the table first — "glyf: glyph 812 runs past its table". */
struct Fault {
    std::string Said;
};

/* What a font file Read takes begins with: a TrueType font's version
 * (0x00010000), Apple's ('true'), a collection's tag ('ttcf'). A file that
 * begins otherwise is no font this reader reads; one that begins so may still
 * be a Fault. Whoever looks for fonts asks for the files that begin so. */
inline constexpr std::string_view FontBeginnings[] = {std::string_view("\0\1\0\0", 4), "true", "ttcf"};

/* The line's measures, in the font's units. Ascender up from the baseline,
 * Descender down from it (negative), LineGap the lead between lines; the
 * typographic ones of OS/2 when the font asks for them, hhea's otherwise.
 * XHeight and CapHeight are 0 when the font does not say them. The lines
 * drawn with text — under it (post) and through it (OS/2) — by where the
 * top of each stroke lies against the baseline, y up, and how thick it is;
 * 0 for what the font does not say. */
struct FontMetrics {
    std::uint16_t UnitsPerEm         = 0;
    std::int16_t  Ascender           = 0;
    std::int16_t  Descender          = 0;
    std::int16_t  LineGap            = 0;
    std::int16_t  XHeight            = 0;
    std::int16_t  CapHeight          = 0;
    std::int16_t  UnderlinePosition  = 0;
    std::int16_t  UnderlineThickness = 0;
    std::int16_t  StrikePosition     = 0;
    std::int16_t  StrikeThickness    = 0;
};

/* What a face is within its family: the family's name ("Inter" — the
 * typographic family when the font names one, its family otherwise), its
 * own name in it ("Bold Italic"), how heavy it is (1 to 1000, 400 regular
 * and 700 bold, as OS/2 says it or head's style when it has no OS/2), and
 * whether it leans — italic or oblique. Names the font does not give are
 * empty. */
struct FontStyle {
    std::string   Family;
    std::string   Name;
    std::uint16_t Weight = 400;
    bool          Italic = false;
};

/* How two glyphs side by side are set closer or apart, in font units: the
 * first moved and its advance changed, the second moved and its advance
 * changed — GPOS's pair adjustment, of which a kern table says only the
 * first's advance. */
struct PairKerning {
    std::int32_t FirstShift    = 0;
    std::int32_t FirstAdvance  = 0;
    std::int32_t SecondShift   = 0;
    std::int32_t SecondAdvance = 0;

    bool operator==(const PairKerning &) const = default;
};

class Font {
public:
    /* The font in Bytes; in a collection (.ttc), the one at Index. */
    static std::expected<Font, Fault> Read(std::vector<std::uint8_t> Bytes, std::uint32_t Index = 0);

    const FontMetrics &Metrics() const noexcept { return Measures; }
    const FontStyle   &Style() const noexcept { return Face; }
    std::uint16_t      GlyphCount() const noexcept { return Glyphs; }

    /* A number no other font read has: what kept pictures of its glyphs are
     * known by. A copy is the same font, and has the same. */
    std::uint64_t Serial() const noexcept { return Number; }

    /* The glyph that draws Character; 0, the font's "no such glyph", when
     * its character map names none. */
    GlyphId Find(char32_t Character) const noexcept;

    /* The glyph's outline, its components put in place. */
    std::expected<Outline, Fault> OutlineOf(GlyphId Glyph) const;

    /* How far the glyph moves the pen. */
    std::uint16_t AdvanceOf(GlyphId Glyph) const noexcept;

    /* Whether the glyph's outline reads; one that does not is drawn by no one. */
    bool Readable(GlyphId Glyph) const noexcept;

    /* How First and then Second are kerned in text of Script (ScriptOf):
     * GPOS's pair adjustments of its 'kern' feature for the script — or for
     * its default, then for Latin, when it says nothing of that script — and
     * when GPOS kerns nothing, the kern table's pairs. None for a font that
     * does not kern. */
    PairKerning KerningOf(GlyphId First, GlyphId Second, Script Of = Script::Common) const noexcept;
    /* Whether it kerns any pair at all. */
    bool Kerns() const noexcept;

    /* The glyphs that do not read, said once: how many, and why the first
     * does not. None for a font that reads whole. */
    const std::optional<Fault> &Damage() const noexcept { return Damaged; }

private:
    /* Where a table, or a part of one, lies in the bytes. */
    struct Span {
        std::uint32_t Offset = 0;
        std::uint32_t Length = 0;
    };

    /* GPOS's kerning lookups — each its pair adjustment subtables, by where
     * they lie in the file — and the lookups each script asks, in order; and
     * the kern table's pairs, each subtable's pairs by where they lie and
     * whether they replace what the subtables before said. */
    struct KerningPlan {
        struct ForScript {
            std::uint32_t              Tag = 0;
            std::vector<std::uint16_t> Lookups;
        };
        std::vector<std::vector<std::uint32_t>> Lookups;
        std::vector<ForScript>                  Scripts;
        struct Pairs {
            std::uint32_t Offset   = 0;
            std::uint32_t Count    = 0;
            bool          Replaces = false;
        };
        std::vector<Pairs> Table;
    };

    Font() = default;
    std::optional<Fault> ReadDirectory(std::uint32_t Index);
    std::optional<Fault> ReadMeasures();
    void                 ReadStyle();
    void                 ReadKerning();
    void                 FindDamage();
    const KerningPlan::ForScript *LookupsFor(Script Of) const noexcept;

    std::vector<std::uint8_t> File;
    Span                      Head, Maxp, Hhea, Hmtx, Loca, Glyf, Cmap, Os2, Name, Post, Gpos, Kern;
    Span                      Map;                     /* the character map's subtable Find reads */
    std::uint16_t             MapFormat   = 0;
    bool                      SymbolMap   = false;     /* a symbol font's: its characters at U+F000 up */
    std::uint16_t             Glyphs      = 0;
    std::uint16_t             Advances    = 0;         /* hmtx's whole records (numberOfHMetrics) */
    bool                      LongOffsets = false;     /* loca of 32-bit offsets */
    FontMetrics               Measures;
    FontStyle                 Face;
    KerningPlan               Kerning;
    std::uint64_t             Number      = 0;
    std::vector<GlyphId>      Unreadable;              /* ascending */
    std::optional<Fault>      Damaged;
};

}  // namespace typecase

#endif  // TYPECASE_FONT_H
