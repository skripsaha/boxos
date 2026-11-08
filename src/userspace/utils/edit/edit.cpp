/* edit — the text of files, written on the glass, each file in a tab.
 *
 *   edit                    the file chooser: a file of the Use Context, or a new one
 *   edit a.txt b.txt        each in a tab of its own, numbered
 *   edit them               what the last command gave
 *   edit notes.txt          a name no file has: an empty tab, the file made by
 *                           its first save, named and tagged on its card
 *
 * Ctrl+? shows every key. What it is made of: text (the lines of a file),
 * history (undo), tab (one open file), keymap (the keys), search and clips
 * (its own windows), paint (the picture), editor (all of it together); and
 * what it shares with read, its kin (../kin): the look, the bars, the keys'
 * help, a field, the file chooser and the file's card, the volume and the
 * clipboard. This part takes what the person typed, and gives back the files
 * that were open.
 */

#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "editor.h"

BOX_FORM("takes optional things \"the files to open\"\n"
         "takes optional words \"the names of new files\"\n"
         "gives things\n");

namespace {

/* The least glass edit is of use on: the tabs, a few lines, the status line,
 * and a window over them. */
constexpr std::int32_t LeastCols = 40;
constexpr std::int32_t LeastRows = 8;

/* Words for the person, before the glass is taken or after it is given back. */
void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    try {
        const file_id_t   *Files     = nullptr;
        std::size_t        FileCount = 0;
        const char *const *Names     = nullptr;
        std::size_t        NameCount = 0;
        if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Names, &NameCount) != 0) return 1;
        if (form_screen_file() != 0) {
            Say("edit writes on the glass: there is nothing it could put into a file through ->");
            return 1;
        }

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("edit"));
            return 1;
        }

        edit::Editor Writer(std::move(*Opened));
        for (std::size_t Index = 0; Index < FileCount; Index++) Writer.Open(Files[Index]);
        for (std::size_t Index = 0; Index < NameCount; Index++) Writer.OpenNew(Names[Index]);
        Writer.Run();

        Say(Writer.Summary());
        const std::vector<file_id_t> Given = Writer.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("edit: there is no memory left for what is open");
        return 1;
    }
}
