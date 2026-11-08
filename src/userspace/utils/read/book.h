#ifndef READ_BOOK_H
#define READ_BOOK_H

/* A file read as a book: its text cut into blocks — paragraphs, headings,
 * quotes, the items of a list, code, tables, rules — each the way it is
 * shown, with what in it looks other than plain.
 *
 * Prose is read the way Markdown writes it, which is also how plain prose is
 * written: lines join into the paragraph they belong to, a blank line ends
 * it, and the marks — `#`, `>`, `- `, `**`, backquotes, [links](...) — become
 * how the text looks rather than characters in it. Lines is every line as it
 * stands, for text whose lines are its sense: code, logs, lists of numbers.
 * Which one a file is read as goes by its name, and the person can say the
 * other.
 *
 * A place in a book is where a block begins in the file: it stays the same
 * place however wide the column is and whichever way the file is read.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"

namespace read {

enum class Kind : std::uint8_t { Paragraph, Heading, Quote, Item, Code, Table, Rule, Line };

enum class Look : std::uint8_t { Plain, Strong, Emphasis, Code, Link };

/* Bytes [From, To) of a block's text, and how they look. */
struct Run {
    std::size_t From = 0;
    std::size_t To   = 0;
    Look        Is   = Look::Plain;
};

struct Block {
    Kind             Is    = Kind::Paragraph;
    std::uint8_t     Level = 0;    /* a heading's 1..6, a quote's depth, an item's depth */
    std::string      Marker;       /* an item's number with its dot; none for a bullet */
    std::string      Text;         /* as shown; a code block's or a table's lines joined by '\n' */
    std::vector<Run> Looks;        /* the stretches that are not plain */
    std::size_t      Source = 0;   /* where it begins in the file */
    std::size_t      Words  = 0;
};

enum class Reading : std::uint8_t { Prose, Lines };

class Book {
public:
    /* The file's text as a book, read as As. */
    static Book Make(file_id_t File, std::string Name, std::string Bytes, Reading As);

    file_id_t          File = 0;
    std::string        Name;
    Reading            As = Reading::Prose;
    std::vector<Block> Blocks;
    std::size_t        Words = 0;

    /* The file's bytes, as the book was made from them. */
    std::string_view FileBytes() const noexcept { return Bytes; }
    /* The same file read the other way. */
    Book Other() const { return Make(File, Name, Bytes, As == Reading::Prose ? Reading::Lines : Reading::Prose); }
    /* The block a place in the file falls in. */
    std::size_t BlockAt(std::size_t Source) const noexcept;
    /* The headings, by their blocks. */
    std::vector<std::size_t> Headings() const;
    /* Every word from block From on. */
    std::size_t WordsFrom(std::size_t From) const noexcept;

private:
    std::string Bytes;
};

/* How a file is read when nobody said: as prose when its name says text —
 * .md, .markdown, .txt, .text, or no ending at all — and as lines otherwise. */
Reading ReadingFor(std::string_view Name) noexcept;

}  // namespace read

#endif
