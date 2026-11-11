/* coinflip — a coin tossed on the glass: it flies up, turns face over face,
 * falls, bounces and lies, heads or tails.
 *
 *   coinflip               one flip
 *   coinflip 10            ten, a little quicker each, and what they came to
 *   coinflip heads         the call made before the toss: said whether it came true
 *   coinflip 10 tails      both
 *
 * Space tosses again, or ends a run of many at once; h and t call; esc
 * leaves. Each flip is one draw of the processor's own entropy, and the coin
 * turns until the face drawn is the one up: what is shown is what was drawn.
 * Under an arrow there is no picture: the faces go to the file, a line each,
 * and the tally of a run after them. settings coinflip sets the metal and
 * the colors around it, how fast and how long a toss is, and the keys. What
 * it is made of: look (the settings), toss (the flight), coin and emblem (the
 * picture of the coin), word (the face's name in big letters), tally (what the
 * flips came to), luck (the draw) and game (the glass and the keys).
 */

#include <charconv>
#include <cstdint>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "game.h"
#include "luck.h"
#include "tally.h"

BOX_FORM("takes optional words \"how many flips, and heads or tails to call before the toss\"\n");

namespace {

using namespace coinflip;

// The least glass the coin is of use on.
constexpr std::int32_t LeastCols = 40;
constexpr std::int32_t LeastRows = 20;
// The most flips a run asks for.
constexpr std::uint64_t MostFlips = 1000000;
// Under an arrow, a run of more than this many gives only its tally.
constexpr std::uint64_t LinesUpTo = 1000;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::string_view Lower(Face Which)
{
    return Which == Face::Heads ? "heads" : "tails";
}

// What the words ask for; the reason, when they ask for what cannot be.
std::optional<std::string> ReadPlan(const char *const *Words, std::size_t Count, Plan &Wanted)
{
    Wanted = Plan{1, std::nullopt};
    for (std::size_t At = 0; At < Count; At++) {
        const std::string_view Word = Words[At];
        if (Word == "heads" || Word == "tails") {
            Wanted.Called = Word == "heads" ? Face::Heads : Face::Tails;
            continue;
        }
        std::uint64_t Flips = 0;
        const auto    Read  = std::from_chars(Word.data(), Word.data() + Word.size(), Flips);
        if (Read.ec != std::errc() || Read.ptr != Word.data() + Word.size())
            return std::format("coinflip: \"{}\" is neither a number of flips nor heads or tails", Word);
        if (Flips == 0 || Flips > MostFlips)
            return std::format("coinflip: a run is from 1 flip to {}, not {}", MostFlips, Flips);
        Wanted.Count = Flips;
    }
    return std::nullopt;
}

// A run as text for a file: each flip's face, and for a run of many the sum.
void PrintRun(const Plan &Wanted)
{
    Luck  Chance;
    Tally Counted;
    std::string Text;
    for (std::uint64_t Flip = 0; Flip < Wanted.Count; Flip++) {
        const Face Landed = Chance.Draw();
        Counted.Add(Landed, Wanted.Called);
        if (Wanted.Count > LinesUpTo) continue;
        Text += Lower(Landed);
        if (Wanted.Called) Text += std::format(" (called {}: {})", Lower(*Wanted.Called), Landed == *Wanted.Called ? "won" : "lost");
        Text += '\n';
    }
    if (Wanted.Count > 1) {
        Text += std::format("heads {}, tails {}, longest run of heads {}, of tails {}\n", Counted.Count(Face::Heads),
                            Counted.Count(Face::Tails), Counted.Longest(Face::Heads), Counted.Longest(Face::Tails));
        if (Wanted.Called) Text += std::format("calls won {} of {}\n", Counted.Won(), Counted.Calls());
    }
    print_bytes(Text.data(), Text.size());
}

}  // namespace

int main()
{
    try {
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        Plan Wanted;
        if (const std::optional<std::string> Refusal = ReadPlan(Words, Count, Wanted)) {
            Say(*Refusal);
            return 1;
        }
        if (form_screen_file() != 0) {
            PrintRun(Wanted);
            return 0;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("coinflip"));
            return 1;
        }
        Game(std::move(*Opened), Wanted).Run();
        return 0;
    } catch (const std::bad_alloc &) {
        Say("coinflip: there is no memory left for the coin");
        return 1;
    }
}
