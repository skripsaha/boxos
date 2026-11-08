/* read — long text, read quietly: edit is for changing a file, read for
 * reading one.
 *
 *   read notes.md           the file, opened where it was left
 *   read a.txt b.txt        one after another, ] and [ between them
 *   read them               what the last command gave
 *
 * The text stands in a column across the middle of the glass, prose wrapped
 * at its words and Markdown shown as what its marks mean; the line being read
 * is lit, and the bottom row says how far through the book is and how long
 * the rest takes. ? shows every key. What it is made of: glyph (characters
 * as cells), book (a file as blocks), layout (blocks as rows), theme (the
 * looks), marks (read.marks, what is remembered), draw and lists (windows),
 * reader and page (all of it together); this part takes the files, and gives
 * back the ones that were read.
 */

#include <cstddef>
#include <new>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "reader.h"

BOX_FORM("takes things \"the files to read, one after another\"\n"
         "gives things\n");

namespace {

/* The least glass a page is of use on. */
constexpr std::int32_t LeastCols = 40;
constexpr std::int32_t LeastRows = 10;

/* Words for the person, before the glass is taken or after it is given back. */
void Say(std::string_view Text)
{
    if (Text.empty()) return;
    std::string Line(Text);
    if (Line.back() != '\n') Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    try {
        const file_id_t *Files     = nullptr;
        std::size_t      FileCount = 0;
        if (form_things(0, &Files, &FileCount) != 0) return 1;
        if (form_screen_file() != 0) {
            Say("read shows a text on the glass: there is nothing it could put into a file through ->");
            return 1;
        }

        read::Memory Remembered;
        const std::optional<std::string> Forgotten = Remembered.Load();

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("read"));
            return 1;
        }

        read::Reader Reader(std::move(*Opened), std::move(Remembered));
        for (std::size_t Index = 0; Index < FileCount; Index++) Reader.Add(Files[Index]);
        Reader.Run();

        if (Forgotten) Say("read: what was remembered could not be read: " + *Forgotten);
        Say(Reader.Afterwords());
        const std::vector<file_id_t> Given = Reader.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("read: there is no memory left for this book");
        return 1;
    }
}
