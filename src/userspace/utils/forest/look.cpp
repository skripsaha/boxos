/* look.cpp — the forest's settings, declared and read. */

#include "look.h"

#include <string>

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("start \"how the forest begins\"\n"
             "    season one-of auto spring summer autumn winter default auto "
             "\"the season: the date's, or the one chosen\"\n"
             "    weather one-of clear rain snow fog wind storm default clear \"the weather it begins with\"\n"
             "    time one-of real chosen default real "
             "\"the time of day: the machine's clock, or a chosen one that starts at the minute below and flows as fast as speed says\"\n"
             "    at number from 0 to 1439 minutes default 480 "
             "\"the minute of the day a chosen time starts at: 480 is eight in the morning\"\n"
             "speed number from 1 to 3600 default 60 "
             "\"how many times faster than the clock a chosen day goes: 60 makes a day of twenty-four minutes\"\n"
             "drift on-or-off default off \"the weather changes by itself now and then\"\n"
             "animals \"who lives in the forest, and how many: as many as that on a glass of 128 x 96 pixels or more, fewer on a smaller one\"\n"
             "    foxes number from 0 to 8 default 2 \"foxes: they trot, sniff, sit, sleep and pounce in the snow\"\n"
             "    hedgehogs number from 0 to 8 default 2 \"hedgehogs: they shuffle at dusk, curl up, sleep all winter\"\n"
             "    hares number from 0 to 10 default 3 \"hares: they sit, wash, hop and bolt; white in winter\"\n"
             "    deer number from 0 to 4 default 1 \"deer: they graze at dawn and dusk\"\n"
             "    birds number from 0 to 16 default 5 \"songbirds: they fly, perch, sing and peck\"\n"
             "    owls number from 0 to 3 default 1 \"owls: they wake at night\"\n"
             "    bats number from 0 to 10 default 3 \"bats: they flit at dusk in the warm months\"\n"
             "    butterflies number from 0 to 24 default 6 \"butterflies: by day in spring and summer\"\n"
             "    fireflies number from 0 to 80 default 24 \"fireflies: on summer nights\"\n"
             "colors \"the forest's own colors\"\n"
             "    sky color default #5AA6E8 \"a clear noon's sky\"\n"
             "    leaves color default #3A8C3E \"the leaves of summer\"\n"
             "    autumn color default #E0902A \"the brightest leaves of autumn\"\n"
             "    blossom color default #F6C8D8 \"the blossom of spring\"\n"
             "    bark color default #6E5230 \"the trunks\"\n"
             "    grass color default #3F9444 \"the meadow\"\n"
             "    snow color default #F2F6FA \"the snow\"\n"
             "bar \"the line at the bottom\"\n"
             "    paper color default #3A3A3A \"behind it\"\n"
             "    text color default #EEEEEE \"what it says\"\n"
             "    keys color default amber \"the keys it names\"\n"
             "keys \"the forest's keys, besides the arrows: left and right turn the time, up and down its speed\"\n"
             "    season key default ctrl+s \"the next season\"\n"
             "    weather key default ctrl+w \"the next weather\"\n"
             "    time key default ctrl+t \"the time: the machine's clock, or the one chosen\"\n"
             "    pause key default ctrl+p \"stop the day, and go on\"\n"
             "    help key default ctrl+? \"the keys, shown and hidden\"\n");

namespace forest {

namespace {

Season SeasonOf(const std::string &Word)
{
    return Word == "spring" ? Season::Spring : Word == "autumn" ? Season::Autumn
         : Word == "winter" ? Season::Winter : Season::Summer;
}

Weather WeatherOf(const std::string &Word)
{
    return Word == "rain" ? Weather::Rain : Word == "snow" ? Weather::Snow : Word == "fog" ? Weather::Fog
         : Word == "wind" ? Weather::Wind : Word == "storm" ? Weather::Storm : Weather::Clear;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    auto Count = [](const char *Path) { return static_cast<std::uint32_t>(settings::number(Path)); };
    auto Paint = [](const char *Path) { return Color::FromRaw(settings::color(Path)); };

    const std::string Season = settings::choice("start season");
    return Look{.SeasonByDate = Season == "auto",
                .StartSeason  = SeasonOf(Season),
                .StartWeather = WeatherOf(settings::choice("start weather")),
                .RealTime     = settings::choice("start time") == "real",
                .StartMinute  = Count("start at"),
                .Speed        = Count("speed"),
                .Drift        = settings::on("drift"),
                .Animals      = {Count("animals foxes"), Count("animals hedgehogs"), Count("animals hares"),
                                 Count("animals deer"), Count("animals birds"), Count("animals owls"),
                                 Count("animals bats"), Count("animals butterflies"), Count("animals fireflies")},
                .Sky          = Paint("colors sky"),
                .Leaves       = Paint("colors leaves"),
                .Autumn       = Paint("colors autumn"),
                .Blossom      = Paint("colors blossom"),
                .Bark         = Paint("colors bark"),
                .Grass        = Paint("colors grass"),
                .Snow         = Paint("colors snow"),
                .BarPaper     = Paint("bar paper"),
                .BarInk       = Paint("bar text"),
                .BarKey       = Paint("bar keys"),
                .Keys         = {settings::key("keys season"), settings::key("keys weather"),
                                 settings::key("keys time"), settings::key("keys pause"), settings::key("keys help")}};
}

}  // namespace forest
