/* casino — a little play casino: a roulette table, a slot machine and a
 * blackjack table, played with chips that are only chips.
 *
 *   casino               at the roulette table
 *   casino slots         at the slot machine
 *   casino blackjack     at the blackjack table
 *
 * Roulette is a single-zero wheel drawn from above with a ball that runs
 * against it, slows, rattles over the frets and drops into a pocket; chips go
 * on a betting board with a cursor — numbers, splits, dozens, columns, red or
 * black, odd or even, low or high — and pay as at any table. The slot machine
 * has three reels of six pictures, up to five lines, a pay table on a key and
 * a jackpot on three sevens. Both chances are drawn from the processor's own
 * entropy where it has any; the odds are shown on the pay table, worked out
 * from the very reels the machine has.
 *
 * The chips are kept on the volume in casino.purse and found again at the next
 * start; a player who has none left may be given a fresh stack, as settings
 * casino says. Tab and 1 2 3 go to another table, ctrl+? shows every key, esc
 * leaves. What it is made of: look (the settings), luck (the draws), purse (the
 * chips), canvas and trig (the pixels), ornament (rules, boxes and cloth drawn
 * with the font), chips, common and shower (what the tables draw alike), wheel,
 * bets and board (roulette's rules and board), roulette and its picture, reels,
 * faces and slots with its picture, cards, hands and blackjack_game (the rules
 * of blackjack), card_art, blackjack and its pictures, and desk (the glass and
 * the keys).
 */

#include <format>
#include <new>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "common.h"
#include "desk.h"

BOX_FORM("takes optional words \"where to sit: roulette, slots or blackjack; at roulette if none\"\n");

namespace {

// The least glass the tables are of use on: the text console's.
constexpr std::int32_t LeastCols = 80;
constexpr std::int32_t LeastRows = 25;

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
        casino::Sitting Start = casino::Sitting::Roulette;
        if (Count > 0) {
            const std::string_view Table = Words[0];
            if (Table == "slots") Start = casino::Sitting::Slots;
            else if (Table == "blackjack") Start = casino::Sitting::Blackjack;
            else if (Table != "roulette") {
                Say(std::format("casino: there is no table called \"{}\": it is roulette, slots or blackjack", Table));
                return 1;
            }
        }
        if (form_screen_file() != 0) {
            Say("casino is played on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("casino"));
            return 1;
        }
        std::string Farewell;
        std::string Fault;
        {
            casino::Desk Floor(std::move(*Opened), Start);
            Floor.Run();
            Farewell = std::format("[casino] {} chips, {}{} this visit", casino::Counted(Floor.Final().Chips),
                                   Floor.Net() > 0 ? "+" : "", casino::Counted(Floor.Net()));
            Fault = Floor.Trouble();
        }
        Say(Farewell);
        if (!Fault.empty()) Say(std::format("casino: the chips are not kept: {}", Fault));
        return 0;
    } catch (const std::bad_alloc &) {
        Say("casino: there is no memory left for the tables");
        return 1;
    }
}
