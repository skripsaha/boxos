/* read — long text, read quietly, each file in a tab: edit is for changing a
 * file, read for reading one, and the two are one family — the same tabs,
 * the same windows, the same keys for the same things.
 *
 *   read                    the file chooser: a file of the Use Context
 *   read notes.md           the file, opened where it was left
 *   read a.txt b.txt        each in a tab of its own, numbered
 *   read them               what the last command gave
 *
 * The text stands in a column across the middle of the glass, prose wrapped
 * at its words and Markdown shown as what its marks mean; the line being read
 * is lit, and the status line says how far through the book is and how long
 * the rest takes. Ctrl+? shows every key. What it is made of: book (a file as
 * blocks), layout (blocks as rows), tab (one open book), marks (read.marks,
 * the bookmarks), keymap (the keys), search and lists (its own windows), page
 * (the picture), reader (all of it together); and what it shares with edit,
 * its kin (../kin). This part takes the files, and gives back the ones that
 * were read.
 */

#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "reader.h"

BOX_FORM("takes optional things \"the files to read, each in a tab of its own\"\n"
         "gives things\n");

namespace {

/* The least glass read is of use on: the tabs, a few lines, the status line,
 * and a window over them. */
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

        read::Memory                     Remembered;
        const std::optional<std::string> Forgotten = Remembered.Load();

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("read"));
            return 1;
        }

        read::Reader Reader(std::move(*Opened), std::move(Remembered));
        for (std::size_t Index = 0; Index < FileCount; Index++) Reader.Open(Files[Index]);
        Reader.Run();

        if (Forgotten) Say("read: the bookmarks could not be read: " + *Forgotten);
        Say(Reader.Afterwords());
        const std::vector<file_id_t> Given = Reader.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("read: there is no memory left for what is open");
        return 1;
    }
}
