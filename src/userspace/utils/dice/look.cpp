/* look.cpp — the dice's settings, declared and read. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("expression text default \"1d6\" "
             "\"what is rolled when dice is started without words: d20, 3d6, 2d6+3, 4d6kh3, adv ...\"\n"
             "animation \"how the dice tumble across the table\"\n"
             "    length number from 200 to 6000 milliseconds or off default 1500 "
             "\"how long the dice tumble before they settle; off: they land at once\"\n"
             "    flips number from 2 to 40 hertz default 14 "
             "\"how often a tumbling die shows another face, slowing as it settles\"\n"
             "    frames number from 10 to 60 hertz default 30 \"how often the table is drawn while they tumble\"\n"
             "colors \"the colors of the table and the dice\"\n"
             "    table color default #1B5E3A \"the table the dice are thrown on\"\n"
             "    body color default #E8E2D0 \"a die\"\n"
             "    ink color default #1A1A1A \"a die's pips and its number\"\n"
             "    high color default #FFC857 \"a highest face: a six on a d6, a 20 on a d20\"\n"
             "    low color default #FF5A5F \"a lowest face: a one\"\n"
             "    bars color default #1E1E1E \"the lines at the top and the bottom\"\n"
             "    panel color default #151515 \"behind the result\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #7C7C7C \"what is said quietly, and the dice that do not count\"\n"
             "keys \"the keys of the table\"\n"
             "    roll key default ctrl+r \"roll the same expression again\"\n"
             "    help key default ctrl+? \"the grammar and every key\"\n");

namespace dice {

Look ReadLook()
{
    using box::settings;
    return Look{.Expression      = settings::text("expression"),
                .TumbleMs        = static_cast<std::uint32_t>(settings::number("animation length")),
                .FlipsPerSecond  = static_cast<std::uint32_t>(settings::number("animation flips")),
                .FramesPerSecond = static_cast<std::uint32_t>(settings::number("animation frames")),
                .Table           = Color::FromRaw(settings::color("colors table")),
                .Body            = Color::FromRaw(settings::color("colors body")),
                .Ink             = Color::FromRaw(settings::color("colors ink")),
                .High            = Color::FromRaw(settings::color("colors high")),
                .Low             = Color::FromRaw(settings::color("colors low")),
                .Bars            = Color::FromRaw(settings::color("colors bars")),
                .Panel           = Color::FromRaw(settings::color("colors panel")),
                .Text            = Color::FromRaw(settings::color("colors text")),
                .Quiet           = Color::FromRaw(settings::color("colors quiet")),
                .Keys            = {settings::key("keys roll"), settings::key("keys help")}};
}

}  // namespace dice
