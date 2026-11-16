/* text.h — the words of one note: lines, a caret, the few ways of changing
 * them, and how they lie on a note of a given width.
 *
 * A place in the text is a line and a byte inside it. A character is one UTF-8
 * sequence, or one byte that is none (painter/tui/text.h); the caret stands
 * between characters. On the note a line that is longer than the note is wide
 * goes on in the next row, broken after a space when it can be: the rows are
 * what Up, Down, Home and End walk, and what the picture shows. */
#ifndef SCRATCHPAD_TEXT_H
#define SCRATCHPAD_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchpad {

// The most a note holds, in bytes: it is for a few lines, not for writing in.
inline constexpr std::size_t MostBytes = 8192;

// One row of the note: the bytes From to To of a line.
struct Row {
    std::size_t Line = 0;
    std::size_t From = 0;
    std::size_t To   = 0;
};

// Where the caret is on the note: its row and its column in cells.
struct CaretSpot {
    std::size_t Row    = 0;
    std::size_t Column = 0;
};

class NoteText {
public:
    NoteText() : Lines(1) {}

    // The text of a file: tabs become blanks, carriage returns go.
    static NoteText FromBytes(std::string_view Bytes);
    // The text as the file holds it: a line to a line, none blank at the end.
    std::string Bytes() const;
    // Nothing in it but blanks.
    bool Blank() const noexcept;
    // Changed since it was made or last said Kept.
    bool Changed() const noexcept { return Edited; }
    void Kept() noexcept { Edited = false; }

    // Type and NewLine: false when the note is full.
    bool Type(std::string_view Typed);
    bool NewLine();
    void Backspace();
    void Delete();

    void Left();
    void Right();
    void Up(std::int32_t Width);
    void Down(std::int32_t Width);
    void RowStart(std::int32_t Width);
    void RowEnd(std::int32_t Width);

    // The rows of the text on a note Width cells wide.
    std::vector<Row> Rows(std::int32_t Width) const;
    std::string_view Line(std::size_t At) const noexcept { return Lines[At]; }
    CaretSpot        Spot(std::int32_t Width) const;

private:
    static constexpr std::size_t NoColumn = static_cast<std::size_t>(-1);

    struct Place {
        std::size_t Line = 0;
        std::size_t Byte = 0;
    };

    std::size_t Before(const std::string &Line, std::size_t Byte) const noexcept;
    std::size_t After(const std::string &Line, std::size_t Byte) const noexcept;
    void        GoToRow(const std::vector<Row> &All, std::size_t Target);
    std::size_t Size() const noexcept;

    std::vector<std::string> Lines;
    Place                    Caret;
    std::size_t              Wanted = NoColumn;   // the column Up and Down keep to
    bool                     Edited = false;
};

}  // namespace scratchpad

#endif
