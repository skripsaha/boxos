/* typing.h — the line at the prompt as typed so far, read the way the Form
 * will cut it at Enter (boxlib form_line.c, which cuts as luggage.c does):
 * blanks separate words; a word that begins with a double quote runs to the
 * next one and is one piece, its quotes no part of it; any other word runs to
 * a blank, and its commas cut it into pieces. A quote inside such a word is a
 * letter like any other. */
#ifndef HUB_TYPING_H
#define HUB_TYPING_H

#include <cstddef>
#include <string_view>
#include <vector>

namespace hub {

struct Typing {
    std::string_view Line;
    std::size_t      Words      = 0;       // whole words before the one being typed; 0: it is the command's name
    std::size_t      NameStart  = 0;       // the command's name, once it is a whole word
    std::size_t      NameEnd    = 0;
    std::size_t      WordStart  = 0;       // the word being typed; Line's end when none is begun
    std::size_t      PieceStart = 0;       // its piece being typed: after its last comma, or its opening quote
    bool             Quoted     = false;   // that piece is inside a quote not yet closed
    bool             AfterArrow = false;   // the whole word before it is ->
    bool             Everywhere = false;   // `everywhere` stands among the whole words

    static Typing Of(std::string_view Line);

    // The command's name: the first word, whole or being typed.
    std::string_view Name() const noexcept;
    // The piece being typed, so far.
    std::string_view Piece() const noexcept { return Line.substr(PieceStart); }
    // Nothing of the piece being typed is typed yet.
    bool Fresh() const noexcept { return PieceStart == Line.size(); }
};

// The pieces of a whole line cut the same way, their quotes left off, none
// empty: `tag a,b "c d"` is tag, a, b and c d.
std::vector<std::string_view> Pieces(std::string_view Line);

}  // namespace hub

#endif
