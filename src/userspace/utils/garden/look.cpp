/* look.cpp — the garden's settings, declared and read. */

#include "look.h"

#include <string>

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("plants number from 3 to 200 plants default 40 \"the most plants the garden shows, the first of the ranking\"\n"
             "wilt number from 1 to 365 days default 14 "
             "\"how long a program may lie unused before its plant is wilted: it wilts the more the longer\"\n"
             "speed number from 10 to 300 percent default 100 \"how fast the wind blows and the wings beat\"\n"
             "order one-of starts recent name default starts "
             "\"what ranks the plants: how often a program was started, how lately, or its name\"\n"
             "counting \"which programs grow here\"\n"
             "    system on-or-off default off \"the machine's own programs — reboot, finish and the like — grow too\"\n"
             "    itself on-or-off default off \"garden's own plant: every look at the garden would grow it\"\n"
             "sky \"the light over the garden\"\n"
             "    time one-of clock day dusk night default clock "
             "\"the hour of the sky: the machine's clock, or always day, dusk or night\"\n"
             "    season one-of clock spring summer autumn winter default clock "
             "\"the season of the plants: the machine's calendar, or always one\"\n"
             "    creatures on-or-off default on \"butterflies and bees by day, fireflies by night, over the plants in bloom\"\n"
             "colors \"the garden's colors\"\n"
             "    day color default #6FB1D8 \"the sky by day\"\n"
             "    dusk color default #C98A62 \"the sky at dusk and dawn\"\n"
             "    night color default #0B1020 \"the sky by night\"\n"
             "    grass color default #4E9A3C \"the grass the plants stand in\"\n"
             "    soil color default #5C4420 \"the soil under it, where the names are\"\n"
             "    leaf color default #3FA34D \"the leaves of a plant in its prime\"\n"
             "    wilted color default #8A6B3A \"the leaves of a plant unused for as long as it can be\"\n"
             "    bloom-a color default #FF7BAC \"a flower\"\n"
             "    bloom-b color default #FFD166 \"a flower\"\n"
             "    bloom-c color default #B39DFF \"a flower\"\n"
             "    fruit color default #E8483C \"the fruit of a tree\"\n"
             "    bars color default #1E1E1E \"the lines at the top and the bottom\"\n"
             "    title color default #EEEEEE \"titles, and the chosen plant's name\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #8C8C8C \"what is said quietly\"\n"
             "    chosen color default #243B5A \"behind the chosen plant's name\"\n"
             "    marked color default #FFC857 \"a marked plant's mark\"\n"
             "keys \"the garden's keys\"\n"
             "    view key default ctrl+l \"the garden, or the list of its plants\"\n"
             "    order key default ctrl+o \"rank the plants the next way: starts, recent, name\"\n"
             "    help key default ctrl+? \"every key of the garden\"\n");

namespace garden {

namespace {

Order OrderOf(const std::string &Word)
{
    return Word == "recent" ? Order::Recent : Word == "name" ? Order::Name : Order::Starts;
}

Hour HourOf(const std::string &Word)
{
    return Word == "day" ? Hour::Day : Word == "dusk" ? Hour::Dusk : Word == "night" ? Hour::Night : Hour::Clock;
}

Season SeasonOf(const std::string &Word)
{
    return Word == "spring" ? Season::Spring : Word == "summer" ? Season::Summer : Word == "autumn" ? Season::Autumn
         : Word == "winter" ? Season::Winter : Season::Clock;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.Shown     = static_cast<std::uint32_t>(settings::number("plants")),
                .WiltDays  = static_cast<std::uint32_t>(settings::number("wilt")),
                .Speed     = static_cast<std::uint32_t>(settings::number("speed")),
                .Ranking   = OrderOf(settings::choice("order")),
                .System    = settings::on("counting system"),
                .Itself    = settings::on("counting itself"),
                .Sky       = HourOf(settings::choice("sky time")),
                .Year      = SeasonOf(settings::choice("sky season")),
                .Creatures = settings::on("sky creatures"),
                .Day       = Color::FromRaw(settings::color("colors day")),
                .Dusk      = Color::FromRaw(settings::color("colors dusk")),
                .Night     = Color::FromRaw(settings::color("colors night")),
                .Grass     = Color::FromRaw(settings::color("colors grass")),
                .Soil      = Color::FromRaw(settings::color("colors soil")),
                .Leaf      = Color::FromRaw(settings::color("colors leaf")),
                .Wilted    = Color::FromRaw(settings::color("colors wilted")),
                .BloomA    = Color::FromRaw(settings::color("colors bloom-a")),
                .BloomB    = Color::FromRaw(settings::color("colors bloom-b")),
                .BloomC    = Color::FromRaw(settings::color("colors bloom-c")),
                .Fruit     = Color::FromRaw(settings::color("colors fruit")),
                .Bars      = Color::FromRaw(settings::color("colors bars")),
                .Title     = Color::FromRaw(settings::color("colors title")),
                .Text      = Color::FromRaw(settings::color("colors text")),
                .Quiet     = Color::FromRaw(settings::color("colors quiet")),
                .Chosen    = Color::FromRaw(settings::color("colors chosen")),
                .Marked    = Color::FromRaw(settings::color("colors marked")),
                .Keys      = {settings::key("keys view"), settings::key("keys order"), settings::key("keys help")}};
}

}  // namespace garden
