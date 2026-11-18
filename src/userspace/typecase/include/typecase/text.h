#ifndef TYPECASE_TEXT_H
#define TYPECASE_TEXT_H

/* A text to be set: its characters, the style it is set in, stretches of it
 * set otherwise, and whether it is secret; and the box it is laid in.
 *
 * Every property of a style is optional: what a span does not say is the
 * text's, and what the text does not say is the system's — the book's
 * default family, 16 pixels to the em, weight 400, upright, no spacing, the
 * font's own line, no lines drawn, the text's own ink. Characters are
 * counted one by one — a character is a Unicode code point — from 0. */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace typecase {

struct Style {
    std::optional<std::string>   Family;          /* a family's name, or several apart by commas: "Inter, Noto Sans" */
    std::optional<float>         Size;            /* pixels to the em */
    std::optional<std::uint16_t> Weight;          /* 1 to 1000: 400 regular, 700 bold */
    std::optional<bool>          Italic;
    std::optional<float>         LetterSpacing;   /* pixels between one character and the next */
    std::optional<float>         LineHeight;      /* the line, times the size; unsaid, the font's own line */
    std::optional<bool>          Underline;
    std::optional<bool>          Strike;          /* a line through */
    std::optional<std::uint32_t> Ink;             /* which of its drawer's inks it is drawn in; 0 the text's own */
};

/* A stretch of the text set otherwise: the characters From up to To. */
struct Span {
    std::uint32_t From = 0;
    std::uint32_t To   = 0;
    typecase::Style Style;
};

struct Text {
    std::u32string    Characters;
    typecase::Style   Style;
    std::vector<Span> Spans;            /* a later one over an earlier where they meet */
    bool              Secret = false;   /* shown as dots, never copied: a password */

    /* Characters given as UTF-8; a byte that does not begin or continue a
     * character is U+FFFD. */
    static Text Of(std::string_view Utf8, typecase::Style Style = {});
};

enum class TextAlign : std::uint8_t { Left, Center, Right, Justify };

/* What becomes of text that does not fit its box: the box grows to hold it;
 * what does not fit is cut off; or cut off, and the last line shown ends
 * in an ellipsis. */
enum class TextOverflow : std::uint8_t { Grow, Clip, Ellipsis };

/* The box a text is laid in. Width none: as wide as the text's longest
 * line, never broken; Height none: as tall as its lines. Lines are broken
 * between words to keep within the width (Wrap), a word wider than the box
 * between its characters; a line break in the text always breaks. A line
 * justified is spread to the width at its spaces, but for a paragraph's
 * last. */
struct TextBox {
    float         Width    = 0;
    float         Height   = 0;
    TextAlign     Align    = TextAlign::Left;
    bool          Wrap     = true;
    std::uint32_t MaxLines = 0;   /* none: any number */
    TextOverflow  Overflow = TextOverflow::Grow;
};

/* Characters as UTF-8. */
std::string Utf8Of(std::u32string_view Characters);
/* UTF-8 as characters, as Text::Of reads it. */
std::u32string CharactersOf(std::string_view Utf8);

}  // namespace typecase

#endif  // TYPECASE_TEXT_H
