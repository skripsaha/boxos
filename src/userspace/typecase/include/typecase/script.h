#ifndef TYPECASE_SCRIPT_H
#define TYPECASE_SCRIPT_H

/* The writing a character belongs to, as far as setting it needs: a font
 * may kern Latin, Greek and Cyrillic each its own way. Common is what every
 * writing shares — spaces, digits, punctuation, symbols — set as the
 * writing round it; Other is any writing a font is asked of by its default.
 * Writings set right to left, and those whose letters change shape with
 * their neighbours, are laid in later (complex scripts): for now they are
 * Other, set left to right letter by letter. */

#include <cstdint>

namespace typecase {

enum class Script : std::uint8_t { Common, Latin, Greek, Cyrillic, Other };

Script ScriptOf(char32_t Character) noexcept;

}  // namespace typecase

#endif  // TYPECASE_SCRIPT_H
