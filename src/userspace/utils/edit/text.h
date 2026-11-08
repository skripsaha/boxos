#ifndef EDIT_TEXT_H
#define EDIT_TEXT_H

/* The text of one file, held as lines.
 *
 * N newlines make N+1 lines, so "a\nb\n" ends in an empty line and joining the
 * lines puts the same bytes back: edit never adds or takes away a byte the
 * person did not type. A file whose every line ends in "\r\n" is held without
 * the carriage returns and written with them again; in any other file a
 * carriage return is a byte of its line like any other.
 *
 * A place in the text is a line and a byte inside it. A character is one UTF-8
 * sequence, or one byte that is none, and a place only ever stands between
 * characters. A column is a cell of the glass: a tab runs to the next stop of
 * four, and a character the font draws in two cells takes two.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kin/glyph.h"

namespace edit {

using namespace kin;

struct Position {
    std::size_t Line = 0;
    std::size_t Byte = 0;

    friend constexpr auto operator<=>(Position, Position) noexcept = default;
};

/* Where Bytes end when they are laid down at At. */
Position After(Position At, std::string_view Bytes) noexcept;

/* What a character is to a jump by words. */
enum class Kind { Blank, Word, Mark };
Kind KindAt(std::string_view Line, std::size_t Byte) noexcept;

class Text {
public:
    static Text FromBytes(std::string_view Bytes);
    std::string   ToBytes() const;
    std::uint64_t ByteCount() const noexcept;

    std::size_t        LineCount() const noexcept { return Lines.size(); }
    const std::string &Line(std::size_t Index) const noexcept { return Lines[Index]; }
    bool               Crlf() const noexcept { return CarriageReturns; }
    /* Changes with every change, so what is worked out from the text knows
     * when to work it out again. */
    std::uint64_t Version() const noexcept { return Changes; }

    Position End() const noexcept;
    Position Clamp(Position At) const noexcept;

    /* The end of what was put in. */
    Position Insert(Position At, std::string_view Bytes);
    /* What was taken out, its lines joined by '\n'. */
    std::string Erase(Position From, Position To);
    std::string Slice(Position From, Position To) const;

    /* One character on, or back; across the end of a line. */
    Position Next(Position At) const noexcept;
    Position Prior(Position At) const noexcept;

    std::size_t ColumnOf(Position At) const noexcept;
    std::size_t ByteAtColumn(std::size_t Line, std::size_t Column) const noexcept;
    std::size_t Width(std::size_t Line) const noexcept;
    /* The blanks a line begins with, in bytes. */
    std::size_t IndentOf(std::size_t Line) const noexcept;

    /* The start of the next word, or of the word this one is in. */
    Position WordAfter(Position At) const noexcept;
    Position WordBefore(Position At) const noexcept;

    /* Where Needle stands next after At, or last before it, going round the
     * text once. Folded: letters A to Z are the same as a to z. */
    std::optional<Position> Find(std::string_view Needle, Position At, bool Forward, bool Folded) const;
    /* Every place Needle stands, in order, none overlapping another. */
    std::vector<Position> FindAll(std::string_view Needle, bool Folded) const;

private:
    std::vector<std::string> Lines{std::string()};
    bool                     CarriageReturns = false;
    std::uint64_t            Changes = 0;
};

}  // namespace edit

#endif
