/* taskboard — what runs on the machine and what it spends, on one board: the
 * load over the last minutes and each core's, the system's cores apart from
 * the programs'; every program — where it is and how it lives, the processor
 * and memory it uses, its strands; memory, the volume and the Use Context; and
 * a journal of the programs started, ended, fallen and raised again.
 *
 *   taskboard              the whole machine
 *   taskboard donut        the programs whose name or tags hold "donut"
 *
 * The chosen program is ended with ctrl+x — asked to finish first, ended at
 * once the second time; the ones marked with space are given back as `them`,
 * their files. ctrl+h shows every key; settings taskboard sets the keys, the
 * colors of a load, how often the machine is read and how far back the graphs
 * look. What it is made of: look (the settings), machine (one reading of the
 * machine), programs (what is kept of them from one reading to the next),
 * history (the load over time), journal (what happened), graph and paint (the
 * picture) and board (all of it together, and the keys). Only graph, paint and
 * board know the screen; this part takes what the person typed and gives back
 * what was marked.
 */

#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "board.h"

BOX_FORM("takes optional name \"what the name or a tag of the programs shown holds\"\n"
         "gives things\n");

namespace {

// The least glass the board is of use on: the programs and the line of keys;
// the load, the cores, the machine and the journal come as room allows.
constexpr std::int32_t LeastCols = 60;
constexpr std::int32_t LeastRows = 16;

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
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        if (form_screen_file() != 0) {
            Say("taskboard is shown on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("taskboard"));
            return 1;
        }
        taskboard::Board Board(std::move(*Opened), Count ? std::string(Words[0]) : std::string());
        Board.Run();
        const std::vector<file_id_t> Given = Board.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("taskboard: there is no memory left for the board");
        return 1;
    }
}
