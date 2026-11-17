#ifndef TYPECASE_CHARACTERS_H
#define TYPECASE_CHARACTERS_H

/* What a character is to a line of text: whether it is drawn, takes room,
 * may end a line or begin one — Unicode's line breaking (UAX #14) in the
 * part that writings set left to right letter by letter need, the rest laid
 * in with complex scripts. */

#include <cstdint>

namespace typecase {

enum class Kind : std::uint8_t {
    Letter,      /* drawn; a line breaks round it only as its neighbours let */
    Space,       /* a space a line may break after (U+0020, U+3000, the typographic spaces) */
    Tab,         /* to the next tab stop */
    LineBreak,   /* ends its line, always: a line feed, CR LF, a line or paragraph separator */
    Invisible,   /* takes no room and draws nothing: controls, joiners, direction marks */
    ZeroSpace,   /* takes no room, a line may break after it (U+200B) */
    SoftHyphen,  /* takes no room, a line may break after it and then shows a hyphen */
    Mark,        /* combining: stays with the character before it */
    Hyphen,      /* '-': a line may break after it, between letters */
    Dash,        /* a hyphen or a dash a line may break after; before too, for the em dash */
    Ideograph,   /* Chinese, Japanese, Korean: a line may break before and after it */
    Opening,     /* opening punctuation: no break after it */
    Closing,     /* closing punctuation, small kana: no break before it */
    Glue,        /* no break before or after it: the no-break spaces, the word joiner */
};

Kind KindOf(char32_t Character) noexcept;

/* Whether a justified line spreads at the character: its spaces. */
bool Spreads(char32_t Character) noexcept;

/* Whether the character draws nothing where it stands — a space, a
 * no-break space, anything that takes no room. */
bool Blank(char32_t Character) noexcept;

/* Whether a line may break between Before and After, Previous being the
 * character before Before (0 for none). */
bool BreaksBetween(char32_t Previous, char32_t Before, char32_t After) noexcept;

/* Whether the caret never stands before the character: it belongs to the
 * one before it — a combining mark, a variation selector, a skin tone, or
 * whatever follows a zero-width joiner (Joined). */
bool Clings(char32_t Character) noexcept;
constexpr char32_t Joiner = 0x200D;

}  // namespace typecase

#endif  // TYPECASE_CHARACTERS_H
