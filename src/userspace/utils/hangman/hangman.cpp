/* hangman — the word-guessing game. The computer picks a word, and the person
 * types its letters, no enter needed: a letter that is in the word fills its
 * places, one that is not puts up a part of the man on the gallows — the head,
 * the body, an arm, the other arm, a leg, the other — and the sixth mistake
 * loses the word. A word found frees the man: the rope is cut and he cheers.
 *
 *   hangman                  words of every category and level
 *   hangman animals          words of that category only
 *   hangman food hard        a category and a level, or a level alone
 *
 * The words are in ten categories (animals, food, countries, science, nature,
 * sports, home, jobs, music, space) and three levels, easy, medium and hard,
 * told by a word's length and the rare letters in it. A hint gives away a
 * letter for some points or for a mistake; a friend can type a secret word,
 * hidden, for the other to guess. Games, wins, the streak and the score are
 * kept in a file of the volume.
 *
 * What it is made of: look (the settings), luck (the chances), words, round
 * (the rules, with nothing of the picture), ledger (the file), scene with
 * backdrop, tones, gallows, flurry and view (the picture), board with slots,
 * lettering and pen, and panels (the words beside it), and game with play (the
 * keys and the rounds).
 */

#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "game.h"

BOX_FORM("takes optional words \"a category to take words from (animals, food, countries, science, nature, sports, home, jobs, music, space), "
         "and a level (easy, medium, hard)\"\n");

namespace {

// The least glass the game is played on: the text console's.
constexpr std::int32_t LeastCols = 80;
constexpr std::int32_t LeastRows = 25;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

// What the words asked at the door mean, or the first that means nothing.
std::optional<std::string> Understood(const char *const *Heard, std::size_t Count, hangman::Request &Asked)
{
    for (std::size_t At = 0; At < Count; At++) {
        const std::string_view Word = Heard[At];
        if (const std::size_t Group = hangman::CategoryNamed(Word); Group < hangman::Categories().size()) {
            if (Asked.Only) return "say one category, not two";
            Asked.Only = Group;
        } else if (const std::optional<hangman::Level> Rank = hangman::LevelNamed(Word)) {
            if (Asked.Hardness) return "say one level, not two";
            Asked.Hardness = Rank;
        } else {
            return std::string(Word) + " is neither a category nor a level";
        }
    }
    return std::nullopt;
}

}  // namespace

int main()
{
    try {
        const char *const *Heard = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Heard, &Count) != 0) return 1;
        hangman::Request Asked;
        if (const std::optional<std::string> Wrong = Understood(Heard, Count, Asked)) {
            Say("hangman: " + *Wrong);
            return 1;
        }
        if (form_screen_file() != 0) {
            Say("hangman is played on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("hangman"));
            return 1;
        }
        hangman::Game Playing(std::move(*Opened), Asked);
        Playing.Run();
        return 0;
    } catch (const std::bad_alloc &) {
        Say("hangman: there is no memory left for the game");
        return 1;
    }
}
