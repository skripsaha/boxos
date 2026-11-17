#ifndef TYPECASE_LAYOUT_H
#define TYPECASE_LAYOUT_H

/* A text laid out in its box: its characters set in the faces their styles
 * come to (fonts.h), kerned, spaced, broken into lines and aligned, every
 * glyph in its place; and what is asked of a text laid — how large it came
 * to, where the caret stands before a character, which character is nearest
 * a point, where a stretch of it lies, the text itself to copy.
 *
 * A line is as tall as the tallest of what is on it: each style's line —
 * its size times its line height, or the font's own line — its ascender
 * and descender in the middle of it, as CSS stands them; a line's height is
 * a whole number of pixels and its baseline on a row's edge, so glyphs land
 * on the rows as they were drawn. Across, every place is where the advances
 * put it, to a fraction of a pixel. Not yet: glyphs that join or change with
 * their neighbours (ligatures), writings set right to left. */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "typecase/fonts.h"
#include "typecase/glyph_look.h"
#include "typecase/size.h"
#include "typecase/text.h"

namespace typecase {

/* Glyphs set alike — of one face at one look, in one ink. */
struct LaidRun {
    std::shared_ptr<const Font> Face;      /* none: characters no font has, each drawn as its code (MissingCell) */
    GlyphLook                   Look;      /* its Phase is the drawer's to set */
    std::uint32_t               Ink = 0;
};

/* A glyph in its place. */
struct LaidGlyph {
    std::uint32_t Run       = 0;   /* of Runs() */
    std::uint32_t Glyph     = 0;   /* the face's glyph; in a run of no face, the character */
    float         X         = 0;   /* its pen, from the layout's left */
    float         Y         = 0;   /* its baseline, from the layout's top */
    std::uint32_t Character = 0;   /* the first character it shows */
};

/* A rectangle of the layout, from its top left. */
struct Area {
    float X      = 0;
    float Y      = 0;
    float Width  = 0;
    float Height = 0;
};

struct LaidLine {
    float         Top        = 0;
    float         Height     = 0;
    float         Baseline   = 0;   /* from the layout's top */
    float         Left       = 0;   /* where its first character stands */
    float         Width      = 0;   /* to the end of its last character, spaces at its end not counted */
    float         Right      = 0;   /* where the pen stands after all of it, spaces at its end and all */
    std::uint32_t FirstGlyph = 0;
    std::uint32_t GlyphCount = 0;
    std::uint32_t From       = 0;   /* its characters: those at its end that draw nothing — spaces, its break — too */
    std::uint32_t To         = 0;
};

/* A line under or through a stretch of text, drawn in the stretch's ink. */
struct LaidDecoration {
    Area          Of;
    std::uint32_t Ink = 0;
};

/* Where the caret stands: a line from Top to Bottom at X, on Line. */
struct Caret {
    float         X      = 0;
    float         Top    = 0;
    float         Bottom = 0;
    std::uint32_t Line   = 0;
};

class Layout {
public:
    static Layout Of(const Text &Set, const TextBox &In, const Fonts &From);

    /* How large it came to: its box where the box says, its lines where not. */
    float Width() const noexcept { return Wide; }
    float Height() const noexcept { return High; }

    const std::vector<LaidLine>       &Lines() const noexcept { return LineList; }
    const std::vector<LaidGlyph>      &Glyphs() const noexcept { return GlyphList; }
    const std::vector<LaidRun>        &Runs() const noexcept { return RunList; }
    const std::vector<LaidDecoration> &Decorations() const noexcept { return DecorationList; }
    const TextBox                     &Box() const noexcept { return Within; }

    /* Whether some of the text does not show: lines left out of the box, or
     * a line cut off at its edge. */
    bool Cut() const noexcept { return CutOff; }

    const std::u32string &Characters() const noexcept { return Said; }
    std::uint32_t         Count() const noexcept { return static_cast<std::uint32_t>(Said.size()); }
    bool                  Secret() const noexcept { return Hidden; }

    /* Where the caret stands before character Index — after the last at
     * Count — a character the caret never stands before (Clings) giving way
     * to the one it belongs to; a character that does not show, where the
     * text is cut. At a line broken between two words, the caret stands at
     * the start of the next. */
    Caret CaretAt(std::uint32_t Index) const noexcept;
    /* The character the caret stands before nearest the point. */
    std::uint32_t IndexAt(float X, float Y) const noexcept;
    /* Where characters From up to To lie: a rectangle on each line they are on. */
    std::vector<Area> Cover(std::uint32_t From, std::uint32_t To) const;
    /* The next place the caret stands after Index, and the one before. */
    std::uint32_t Next(std::uint32_t Index) const noexcept;
    std::uint32_t Previous(std::uint32_t Index) const noexcept;
    /* Characters From up to To as UTF-8 — none of a secret text. */
    std::string Copy(std::uint32_t From, std::uint32_t To) const;

private:
    friend struct Setter;

    std::u32string              Said;
    bool                        Hidden = false;
    TextBox                     Within;
    float                       Wide   = 0;
    float                       High   = 0;
    bool                        CutOff = false;
    std::vector<LaidLine>       LineList;
    std::vector<LaidGlyph>      GlyphList;
    std::vector<LaidRun>        RunList;
    std::vector<LaidDecoration> DecorationList;
    std::vector<float>          Stops;    /* where the caret stands before each character, and after the last */
    std::vector<std::uint32_t>  LineOf;   /* the line of each, and of the end */
};

/* The cell a character no font has is drawn in as its code, at Em: an em
 * tall, its baseline four fifths of the way down, as wide as its digits
 * need. Its advance is the cell's width and a tenth of the em. */
Size  MissingCell(char32_t Character, float Em) noexcept;
float MissingAdvance(char32_t Character, float Em) noexcept;

}  // namespace typecase

#endif  // TYPECASE_LAYOUT_H
