/* look.cpp — the casino's settings, declared and read. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("chips \"the play chips: they are never money\"\n"
             "    start number from 10 to 1000000 chips default 1000 \"the stack a new player is given\"\n"
             "    refill on-or-off default on \"a player who has none left is given a fresh stack\"\n"
             "    refill-chips number from 10 to 1000000 chips default 500 \"how many chips the fresh stack holds\"\n"
             "    step number from 1 to 1000 chips default 5 \"the smallest chip; the others are 2, 5, 10, 20, 50 "
             "and 100 times it\"\n"
             "speed number from 25 to 400 percent default 100 \"how fast the wheel and the reels turn\"\n"
             "blackjack \"the rules of the blackjack table, taken for the next round\"\n"
             "    decks number from 1 to 8 default 6 \"how many decks the shoe holds\"\n"
             "    soft-17 one-of stand hit default stand \"what the dealer does on a soft 17\"\n"
             "    double one-of any nine-to-eleven default any \"double down on any two cards, or only on a hard 9, 10 or 11\"\n"
             "    double-after-split on-or-off default on \"a hand of a split may be doubled\"\n"
             "    hands number from 2 to 4 default 4 \"the most hands a player may make by splitting\"\n"
             "    resplit-aces on-or-off default off \"split aces may be split again\"\n"
             "    surrender on-or-off default off \"give up a hand for half the bet before playing it\"\n"
             "    insurance on-or-off default on \"insurance is offered when the dealer shows an ace\"\n"
             "colors \"the colors of the casino\"\n"
             "    page color default #0C0C0C \"behind everything\"\n"
             "    felt color default #0F6B33 \"the table's cloth\"\n"
             "    wheel color default #8A5224 \"the wheel's wood\"\n"
             "    cabinet color default #A8262C \"the slot machine's body\"\n"
             "    rail color default #8B5A1E \"the wood round the tables\"\n"
             "    card color default #F6F2E6 \"the white of a playing card\"\n"
             "    card-back color default #B22B33 \"the pattern on the back of a card\"\n"
             "    lights color default #FFC857 \"the bulbs round the table and the cabinet\"\n"
             "    red color default #C8282E \"the red pockets and cells\"\n"
             "    black color default #1A1A1A \"the black pockets and cells\"\n"
             "    gold color default #F2C14E \"trim, titles and the jackpot\"\n"
             "    text color default #D8D8D8 \"what is said\"\n"
             "    quiet color default #8A8A8A \"what is said quietly\"\n"
             "    won color default #8BE38B \"a win\"\n"
             "    lost color default #FF6B6B \"a loss\"\n"
             "keys \"the casino's keys\"\n"
             "    spin key default ctrl+s \"spin the wheel or the reels, as space does\"\n"
             "    table key default ctrl+t \"go to the other table\"\n"
             "    pay key default ctrl+p \"the pay table, or the payouts of the roulette board\"\n"
             "    help key default ctrl+? \"every key of the casino\"\n");

namespace casino {

namespace {

BlackjackRules ReadRules()
{
    using box::settings;
    return BlackjackRules{.Decks            = static_cast<int>(settings::number("blackjack decks")),
                          .HitSoft17        = settings::choice("blackjack soft-17") == "hit",
                          .DoubleAny        = settings::choice("blackjack double") == "any",
                          .DoubleAfterSplit = settings::on("blackjack double-after-split"),
                          .MaxHands         = static_cast<int>(settings::number("blackjack hands")),
                          .ResplitAces      = settings::on("blackjack resplit-aces"),
                          .Surrender        = settings::on("blackjack surrender"),
                          .Insurance        = settings::on("blackjack insurance")};
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.StartChips  = settings::number("chips start"),
                .Refill      = settings::on("chips refill"),
                .RefillChips = settings::number("chips refill-chips"),
                .Step        = settings::number("chips step"),
                .Speed       = static_cast<std::int32_t>(settings::number("speed")),
                .Page        = Color::FromRaw(settings::color("colors page")),
                .Felt        = Color::FromRaw(settings::color("colors felt")),
                .Wheel       = Color::FromRaw(settings::color("colors wheel")),
                .Cabinet     = Color::FromRaw(settings::color("colors cabinet")),
                .Rail        = Color::FromRaw(settings::color("colors rail")),
                .CardFace    = Color::FromRaw(settings::color("colors card")),
                .CardBack    = Color::FromRaw(settings::color("colors card-back")),
                .Lights      = Color::FromRaw(settings::color("colors lights")),
                .Red         = Color::FromRaw(settings::color("colors red")),
                .Black       = Color::FromRaw(settings::color("colors black")),
                .Gold        = Color::FromRaw(settings::color("colors gold")),
                .Text        = Color::FromRaw(settings::color("colors text")),
                .Quiet       = Color::FromRaw(settings::color("colors quiet")),
                .Won         = Color::FromRaw(settings::color("colors won")),
                .Lost        = Color::FromRaw(settings::color("colors lost")),
                .Blackjack   = ReadRules(),
                .Keys        = {settings::key("keys spin"), settings::key("keys table"), settings::key("keys pay"),
                                settings::key("keys help")}};
}

}  // namespace casino
