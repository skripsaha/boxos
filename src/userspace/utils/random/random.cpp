/* random — a number, or one of a few things, chosen by chance.
 *
 *   random                       a number from 1 to 100 (settings random: range)
 *   random 1000                  a number from 1 to 1000
 *   random 1 6                   a number from 1 to 6
 *   random apple pear plum       one of the words
 *   random them                  one of the files the last command gave —
 *                                and it is given back as `them`, for `show them`
 *   random 5 of 1 49             five numbers, each drawn as if for the first time
 *   random 6 different of 1 49   six that are all different, as a lottery draws
 *   random 2 different of them   two files, never the same twice
 *   random shuffle a b c d       all of them, in a new order
 *   random shuffle them          the files, in a new order, given back in it
 *
 * The first words say how many (parts: request.h), then what from. Three
 * numbers or more are words to choose among — dice with odd faces — and two
 * are a range. Words and files are not mixed. The choice is said one to a line;
 * at the screen it first rolls like the reels of a slot machine and settles
 * (reveal.cpp; settings random: animation, length, colors), and into a file
 * through -> it is only said. Where it comes from: source.h. Files chosen are
 * given back; numbers and words are only said, so `them` after them is empty. */

#include <algorithm>
#include <expected>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"

#include "draw.h"
#include "look.h"
#include "request.h"
#include "reveal.h"
#include "source.h"

BOX_FORM("takes optional words \"how many — 3 of, 3 different of, shuffle — then a range 1 6 or 1000, or the words to "
         "choose among; none: a number from 1 to 100\"\n"
         "takes optional things \"the files to choose among: them, files, everywhere all\"\n"
         "gives things\n");

namespace {

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

// Only the files, each once, in the order they were chosen.
std::vector<file_id_t> FilesOf(const std::vector<random_pick::Drawn> &Chosen)
{
    std::vector<file_id_t> Files;
    for (const random_pick::Drawn &One : Chosen)
        if (One.File != 0 && std::find(Files.begin(), Files.end(), One.File) == Files.end()) Files.push_back(One.File);
    return Files;
}

// Whether the roll is shown: a person is at the screen, and what is said
// is not going into a file.
bool Rolls(const random_pick::Look &Shown)
{
    return Shown.Animated && form_person_here() && form_screen_file() == 0;
}

int Run()
{
    const char *const *Words;
    std::size_t        WordCount;
    const file_id_t   *Files;
    std::size_t        FileCount;
    if (form_words(0, &Words, &WordCount) != 0 || form_things(1, &Files, &FileCount) != 0) return 1;

    const random_pick::Look Shown = random_pick::ReadLook();
    const std::expected<random_pick::Request, std::string> Asked =
        random_pick::Read(std::vector<std::string>(Words, Words + WordCount),
                          std::vector<file_id_t>(Files, Files + FileCount), {Shown.Least, Shown.Most});
    if (!Asked) {
        Say("random: " + Asked.error());
        return 1;
    }

    random_pick::Source                  Random;
    const std::vector<random_pick::Drawn> Chosen = random_pick::Draw(*Asked, Random);
    if (!Rolls(Shown) || !random_pick::Reveal(*Asked, Chosen, Shown, Random)) {
        std::string Said;
        for (const random_pick::Drawn &One : Chosen) Said += One.Text + '\n';
        print_bytes(Said.data(), Said.size());
    }
    const std::vector<file_id_t> Given = FilesOf(Chosen);
    if (!Given.empty()) souvenir_things(Given.data(), Given.size());
    return 0;
}

}  // namespace

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        Say("random: there is no memory left to choose in");
        return 1;
    }
}
