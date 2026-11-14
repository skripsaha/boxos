/* garden — how much each program is used, grown as a garden. Every program
 * the person starts is a plant: the more often it is started the taller it
 * grows — from a sprout to a tree — and the longer it lies unused the more it
 * wilts. Flowers and fruit, butterflies, bees and fireflies, the hour of the
 * sky and the season are the machine's own; the plants are ranked from the
 * left, and the chosen one says its name, how often it was started and when.
 *
 *   garden              the whole garden
 *   garden donut        the plant whose name holds "donut" chosen
 *
 * The counting is the gardener's (utility gardener), which lives in the
 * background and keeps garden.plants on the volume; the garden reads it, and
 * grows as the gardener tells it of a start. The plants marked with space are
 * given back as `them`, their program files. settings garden sets the colors,
 * the sky, the season, what counts, how fast the wind blows and the keys.
 * What it is made of: record (the file), growth (what a plant is, and the
 * ranking), sky, flora and air (the picture's parts), paint and view (all of
 * it together, and the keys). Only the painting parts know the screen; this
 * part takes what the person typed and gives back what was marked.
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"
#include "box/form.h"
#include "box/print.h"
#include "box/program_part.h"
#include "painter/tui/stage.h"

#include "view.h"

BOX_FORM("takes optional name \"what the name of the plant chosen holds\"\n"
         "gives things\n");

namespace {

// The least glass the garden is of use on: the sky, a plant and its words,
// and the two lines under them.
constexpr std::int32_t LeastCols = 56;
constexpr std::int32_t LeastRows = 16;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

// The program files of the plants Named: a program is a file that holds a box
// or an ELF image, called by the name without its extension.
std::vector<file_id_t> ProgramsNamed(const std::vector<std::string> &Named)
{
    std::vector<file_id_t> Files;
    for (const std::string &Plant : Named) {
        file_id_t *Ids   = nullptr;
        const int  Count = query_all_everywhere(Plant.c_str(), "trashed", &Ids);
        for (int At = 0; At < Count; At++) {
            ProgramKind Kind = PROGRAM_NONE;
            if (program_kind(Ids[At], &Kind) != 0 || Kind == PROGRAM_NONE) continue;
            FileMetadata Metadata;
            if (FileMetadataRead(Ids[At], &Metadata) != 0) continue;
            std::string Name = Metadata.Filename ? Metadata.Filename : "";
            FileMetadataRelease(&Metadata);
            if (const std::size_t Dot = Name.rfind('.'); Dot != std::string::npos && Dot > 0) Name.resize(Dot);
            if (Name == Plant && std::find(Files.begin(), Files.end(), Ids[At]) == Files.end())
                Files.push_back(Ids[At]);
        }
        free(Ids);
    }
    return Files;
}
}  // namespace

int main()
{
    try {
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        if (form_screen_file() != 0) {
            Say("garden is shown on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("garden"));
            return 1;
        }
        garden::View Garden(std::move(*Opened), Count ? std::string(Words[0]) : std::string());
        Garden.Run();
        const std::vector<file_id_t> Given = ProgramsNamed(Garden.Given());
        if (!Given.empty()) souvenir_things(Given.data(), Given.size());
        return 0;
    } catch (const std::bad_alloc &) {
        Say("garden: there is no memory left for the garden");
        return 1;
    }
}
