#ifndef READ_READER_H
#define READ_READER_H

/* read as a whole: the books, the page, the keys, and what is remembered.
 *
 * The page is a book laid in a column across the middle of the glass: above
 * it the book's name and the heading the reader is under, below it how far
 * through the book is, which page, and how long is left to read. One line of
 * it is the reading line — the arrows move it, the page follows it, Ctrl+C
 * copies its paragraph when nothing is selected, and m puts a bookmark on it.
 * Several books are read one after another, ] and [ between them, each opened
 * where it was left. A window — the contents, the bookmarks, the keys — is in
 * front while it is open, and the keys are its own; a search is typed on the
 * bottom row and found as it is typed.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "box/file.h"
#include "painter/tui/stage.h"

#include "book.h"
#include "draw.h"
#include "layout.h"
#include "lists.h"
#include "marks.h"
#include "theme.h"

namespace read {

class Reader {
public:
    Reader(tui::Stage Screen, Memory Remembered);

    /* A file to read, after those already given. */
    void Add(file_id_t File) { Files.push_back(File); }
    void Run();

    /* The files that were opened and read, for whoever started read. */
    std::vector<file_id_t> Given() const { return Opened; }
    /* What is to be said once the glass is given back: what would not open,
     * what could not be remembered. */
    std::string            Afterwords() const;

private:
    /* The column when nobody said: as wide as prose reads well. */
    static constexpr std::int32_t UsualWidth = 72;
    static constexpr std::int32_t LeastWidth = 24;
    static constexpr std::int32_t WidthStep  = 4;

    enum class Mode { Reading, Finding, Listing };
    enum class Window { Contents, Bookmarks, Keys };

    struct Match {
        std::size_t Block = 0;
        std::size_t From  = 0;
        std::size_t To    = 0;
    };

    /* reader.cpp — books and places */
    bool        Show(std::size_t Index);
    void        Leave();
    void        Lay();
    void        Relay();
    std::size_t PlaceOfReading() const;
    void        GoToBlock(std::size_t Block, std::size_t Byte = 0);

    /* reader.cpp — moving */
    bool TextRow(std::size_t Row) const noexcept;
    void Step(bool Down, bool Extend);
    void Page(bool Down);
    void Edge(bool End);
    void Heading(bool Next);
    void Follow() noexcept;
    std::size_t MostTop() const noexcept;

    /* reader.cpp — keys */
    void OnKey(const tui::KeyEvent &Key);
    void OnReading(const tui::KeyEvent &Key);
    void OnFinding(const tui::KeyEvent &Key);
    void OnListing(const tui::KeyEvent &Key);
    void OpenList(Window Which);
    void ToggleMark();
    void Copy();
    void Widen(std::int32_t By);
    void Note(std::string Words, bool Trouble = false);

    /* reader.cpp — finding */
    void Search();
    void Find();
    void NextMatch(bool Forward);
    void ShowMatch();

    /* page.cpp — the picture */
    bool         Paint();
    void         PaintTitle(tui::Frame &Out) const;
    void         PaintRow(tui::Frame &Out, std::int32_t Y, std::size_t Row) const;
    void         PaintText(tui::Frame &Out, std::int32_t Y, std::size_t Row, core::Color Paper) const;
    void         PaintStatus(tui::Frame &Out) const;
    std::int32_t Column() const noexcept;
    std::int32_t Left() const noexcept;
    std::size_t  PageRows() const noexcept;
    /* A page turns by its rows less one, the one kept for the eye: pages are
     * counted by the same step. */
    std::size_t  Stride() const noexcept { return PageRows() > 1 ? PageRows() - 1 : 1; }
    std::size_t  Pages() const noexcept { return (MostTop() + Stride() - 1) / Stride() + 1; }
    std::size_t  PageOf(std::size_t Row) const noexcept { return std::min(Row / Stride() + 1, Pages()); }
    std::string  HeadingAbove() const;
    std::string  Progress() const;

    tui::Stage   Screen;
    core::Size   Bounds;
    Memory       Remembered;
    const Theme *Scheme;                     /* the theme the page is in */
    std::int32_t Width = 0;                  /* the column the person asked for; 0: the usual */

    std::vector<file_id_t>   Files;
    std::vector<file_id_t>   Opened;
    std::vector<std::string> Troubles;
    std::size_t              Current = 0;
    Book                     Here;
    Layout                   Lines;
    std::vector<std::size_t> Marked;         /* the bookmarks of the book, by place */

    std::size_t                Top     = 0;  /* the first row on the page */
    std::size_t                Reading = 0;  /* the reading line */
    std::optional<std::size_t> Anchor;       /* the other end of the selection */

    Mode               Now = Mode::Reading;
    Window             Front = Window::Contents;
    List               Shown;
    std::string        Needle;
    std::vector<Match> Matches;
    std::size_t        AtMatch = 0;

    std::string Message;
    bool        MessageTrouble = false;
    bool        Done = false;
};

}  // namespace read

#endif
