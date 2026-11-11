/* scratchpad — short quick notes, each on a sticky note of its own color.
 *
 *   scratchpad              a new note, its paper chosen at random
 *   scratchpad idea         the note called idea; a name no note has, a new note called so
 *   scratchpad them         what the last command gave, the notes paged through
 *
 * Esc saves the notes and leaves; a note with nothing in it is not saved. A
 * note is a file wearing the tag scratchpad, and its colors the tags paper and
 * ink, so `files scratchpad` lists the notes and any other program reads one as
 * the text it is. Ctrl+? shows every key; settings scratchpad sets the papers a
 * new note may take or the one it must, the ink, the size of the note and the
 * keys. What it is made of: look (the settings and the choice of a paper), text
 * (the words of a note), notes (a note and its file), paint (the picture) and
 * pad (all of it together, and the keys). This part takes what the person typed
 * and gives back the notes that were open.
 */

#include <cstddef>
#include <expected>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "pad.h"

BOX_FORM("takes optional things \"the notes to open\"\n"
         "takes optional words \"the names of new notes\"\n"
         "gives things\n");

namespace {

// The least glass the pad is of use on: a note of the least size and its margin.
constexpr std::int32_t LeastCols = 26;
constexpr std::int32_t LeastRows = 8;

// Words for the person, before the glass is taken or after it is given back.
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
            Say("scratchpad is shown on the glass: there is nothing it could put into a file through ->");
            return 1;
        }

        const scratchpad::Look        Shown = scratchpad::ReadLook();
        std::vector<scratchpad::Note> Opened;
        for (std::size_t Index = 0; Index < FileCount; Index++) {
            std::expected<scratchpad::Note, std::string> Read = scratchpad::Note::Read(Files[Index], Shown);
            if (Read) Opened.push_back(std::move(*Read));
            else Say("scratchpad: " + Read.error());
        }
        if (FileCount > 0 && Opened.empty() && NameCount == 0) return 1;
        for (std::size_t Index = 0; Index < NameCount; Index++)
            Opened.push_back(scratchpad::Note::Blank(Names[Index], Shown));
        if (Opened.empty()) Opened.push_back(scratchpad::Note::Blank({}, Shown));

        std::expected<painter::tui::Stage, painter::tui::StageError> Glass =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Glass) {
            Say(Glass.error().Describe("scratchpad"));
            return 1;
        }
        scratchpad::Pad Pad(std::move(*Glass), std::move(Opened));
        Pad.Run();

        if (const std::string Summary = Pad.Summary(); !Summary.empty()) Say(Summary);
        const std::vector<file_id_t> Given = Pad.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("scratchpad: there is no memory left for the notes");
        return 1;
    }
}
