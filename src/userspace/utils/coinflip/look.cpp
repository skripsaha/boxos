/* look.cpp — the coin's settings, declared and read. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("coin \"the coin itself\"\n"
             "    metal color default #D9A93A \"what it is struck from: gold, silver (#C4C8D0), copper (#B8693D) "
             "or any color; its rim and shading are the dark of it\"\n"
             "    size number from 20 to 100 percent default 60 \"how much of the room it takes\"\n"
             "colors \"the colors around the coin\"\n"
             "    page color default #0E0F16 \"behind the coin\"\n"
             "    floor color default #1D2030 \"what it falls on\"\n"
             "    text color default #D0D0D0 \"what is said\"\n"
             "    quiet color default #7C7C7C \"what is said quietly\"\n"
             "    heads color default #FFD166 \"the word HEADS\"\n"
             "    tails color default #9DB4D0 \"the word TAILS\"\n"
             "    won color default #8BE38B \"a call that came true\"\n"
             "    lost color default #FF6B6B \"a call that did not\"\n"
             "toss \"how the coin is tossed\"\n"
             "    speed number from 25 to 400 percent default 100 \"how fast a toss goes: 100 is two seconds from "
             "the hand to rest\"\n"
             "    quicker number from 100 to 1000 percent default 300 \"how much quicker the flips are when a count "
             "is asked, coinflip 10\"\n"
             "    spins number from 1 to 20 turns default 6 \"how many turns it makes in the air\"\n"
             "    bounces number from 0 to 5 default 3 \"how many times it bounces before it lies\"\n"
             "keys \"the keys of the coin\"\n"
             "    flip key default ctrl+f \"toss it again\"\n"
             "    heads key default alt+h \"call heads, and toss\"\n"
             "    tails key default alt+t \"call tails, and toss\"\n"
             "    help key default ctrl+? \"every key of the coin\"\n");

namespace coinflip {

Look ReadLook()
{
    using box::settings;
    const Color Metal = Color::FromRaw(settings::color("coin metal"));
    return Look{.Metal    = Metal,
                .Edge     = Color::Mix(Metal, painter::core::Colors::Black, 175),
                .Page     = Color::FromRaw(settings::color("colors page")),
                .Floor    = Color::FromRaw(settings::color("colors floor")),
                .Text     = Color::FromRaw(settings::color("colors text")),
                .Quiet    = Color::FromRaw(settings::color("colors quiet")),
                .HeadsInk = Color::FromRaw(settings::color("colors heads")),
                .TailsInk = Color::FromRaw(settings::color("colors tails")),
                .Won      = Color::FromRaw(settings::color("colors won")),
                .Lost     = Color::FromRaw(settings::color("colors lost")),
                .Size     = static_cast<std::int32_t>(settings::number("coin size")),
                .Speed    = static_cast<std::int32_t>(settings::number("toss speed")),
                .Quicker  = static_cast<std::int32_t>(settings::number("toss quicker")),
                .Spins    = static_cast<std::int32_t>(settings::number("toss spins")),
                .Bounces  = static_cast<std::int32_t>(settings::number("toss bounces")),
                .Keys     = {settings::key("keys flip"), settings::key("keys heads"), settings::key("keys tails"),
                             settings::key("keys help")}};
}

}  // namespace coinflip
