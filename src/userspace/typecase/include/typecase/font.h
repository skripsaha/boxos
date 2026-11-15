#ifndef TYPECASE_FONT_H
#define TYPECASE_FONT_H

/* A TrueType font, read from its file's bytes: which glyph draws a
 * character, the glyph's outline, how far it moves the pen, and the
 * measures of the line.
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
#include <vector>

#include "typecase/outline.h"

namespace typecase {

using GlyphId = std::uint16_t;

/* Why a font, or one of its glyphs, cannot be read: a sentence for a
 * person, the table first — "glyf: glyph 812 runs past its table". */
struct Fault {
    std::string Said;
};

/* The line's measures, in the font's units. Ascender up from the baseline,
 * Descender down from it (negative), LineGap the lead between lines; the
 * typographic ones of OS/2 when the font asks for them, hhea's otherwise.
 * XHeight and CapHeight are 0 when the font does not say them. */
struct FontMetrics {
    std::uint16_t UnitsPerEm = 0;
    std::int16_t  Ascender   = 0;
    std::int16_t  Descender  = 0;
    std::int16_t  LineGap    = 0;
    std::int16_t  XHeight    = 0;
    std::int16_t  CapHeight  = 0;
};

class Font {
public:
    /* The font in Bytes; in a collection (.ttc), the one at Index. */
    static std::expected<Font, Fault> Read(std::vector<std::uint8_t> Bytes, std::uint32_t Index = 0);

    const FontMetrics &Metrics() const noexcept { return Measures; }
    std::uint16_t      GlyphCount() const noexcept { return Glyphs; }

    /* The glyph that draws Character; 0, the font's "no such glyph", when
     * its character map names none. */
    GlyphId Find(char32_t Character) const noexcept;

    /* The glyph's outline, its components put in place. */
    std::expected<Outline, Fault> OutlineOf(GlyphId Glyph) const;

    /* How far the glyph moves the pen. */
    std::uint16_t AdvanceOf(GlyphId Glyph) const noexcept;

    /* Whether the glyph's outline reads; one that does not is drawn by no one. */
    bool Readable(GlyphId Glyph) const noexcept;

    /* The glyphs that do not read, said once: how many, and why the first
     * does not. None for a font that reads whole. */
    const std::optional<Fault> &Damage() const noexcept { return Damaged; }

private:
    /* Where a table, or a part of one, lies in the bytes. */
    struct Span {
        std::uint32_t Offset = 0;
        std::uint32_t Length = 0;
    };

    Font() = default;
    std::optional<Fault> ReadDirectory(std::uint32_t Index);
    std::optional<Fault> ReadMeasures();
    void                 FindDamage();

    std::vector<std::uint8_t> File;
    Span                      Head, Maxp, Hhea, Hmtx, Loca, Glyf, Cmap, Os2;
    Span                      Map;                     /* the character map's subtable Find reads */
    std::uint16_t             MapFormat   = 0;
    bool                      SymbolMap   = false;     /* a symbol font's: its characters at U+F000 up */
    std::uint16_t             Glyphs      = 0;
    std::uint16_t             Advances    = 0;         /* hmtx's whole records (numberOfHMetrics) */
    bool                      LongOffsets = false;     /* loca of 32-bit offsets */
    FontMetrics               Measures;
    std::vector<GlyphId>      Unreadable;              /* ascending */
    std::optional<Fault>      Damaged;
};

}  // namespace typecase

#endif  // TYPECASE_FONT_H
