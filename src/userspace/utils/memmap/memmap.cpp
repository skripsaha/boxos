/* memmap — all the machine's memory as one map: every cell a run of pages,
 * colored by who holds it — the kernel and its kinds, each program by name,
 * what is shared, the free — beside a legend of what each holds, with what an
 * owner has gained since the map opened. The cursor tells what a cell is; the
 * map reads the machine again as it goes, so a program that starts, or one
 * that only grows, shows where it happens.
 *
 *   memmap              the whole machine
 *   memmap donut        the owners whose name or tags hold "donut" stand out
 *
 * Two views, ctrl+v between them: by owner, memory cut owner after owner from
 * what the system counts — what each program has mapped, what is in use and no
 * program maps, what is free — with the kernel's image, rings and shared
 * windows carved out of the kernel's share by what the registry of tagged
 * memory (MemTag) names; and by address, the address space as that registry
 * tells it, where a ring or a window sits and where it says no more than the
 * zone. The registry does not say which pages a program's heap or the kernel's
 * own heap are on, nor which are free, so the address view shows no more than
 * it knows. The program marked with space is given back as `them`, its file;
 * settings memmap sets the keys, the colors and how the cells are drawn.
 * What it is made of: look (the settings), machine (one reading of the
 * machine), owners (who holds what, and what each gained), mosaic (the cells),
 * paint (the picture) and board (all of it together, and the keys). Only paint
 * and board know the screen; this part takes what the person typed and gives
 * back what was marked.
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

BOX_FORM("takes optional name \"what the name or a tag of the owners that stand out holds\"\n"
         "gives things\n");

namespace {

// The least glass the map is of use on: a few cells, their owners and the
// line of keys.
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
            Say("memmap is shown on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("memmap"));
            return 1;
        }
        memmap::Board Board(std::move(*Opened), Count ? std::string(Words[0]) : std::string());
        Board.Run();
        const std::vector<file_id_t> Given = Board.Given();
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("memmap: there is no memory left for the map");
        return 1;
    }
}
