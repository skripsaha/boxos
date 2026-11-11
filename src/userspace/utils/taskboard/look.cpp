/* look.cpp — the board's settings, declared and read. */

#include "look.h"

#include <algorithm>
#include <string>

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("refresh number from 250 to 10000 milliseconds default 1000 \"how often the machine is read again\"\n"
             "history number from 30 to 3600 seconds default 120 \"how far back the load graphs reach\"\n"
             "load \"the colors of a load, in the graphs and meters\"\n"
             "    cool color default #2EC4B6 \"a light load\"\n"
             "    warm color default amber \"a load halfway\"\n"
             "    hot color default #FF5A5F \"a full load\"\n"
             "colors \"the board's own colors\"\n"
             "    page color default #0F0F0F \"behind the panels\"\n"
             "    panel color default #151515 \"inside a panel\"\n"
             "    bars color default #1E1E1E \"the lines at the top and the bottom\"\n"
             "    frame color default #464646 \"a panel's frame\"\n"
             "    title color default #EEEEEE \"titles, and the chosen program's name\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #7C7C7C \"what is said quietly\"\n"
             "    chosen color default #243B5A \"behind the chosen program\"\n"
             "    marked color default #FFC857 \"a marked program's mark\"\n"
             "places \"the colors of where a program is and how it lives\"\n"
             "    on-screen color default #5FD7FF \"its screen is the one shown\"\n"
             "    behind color default #B39DFF \"let go: it lives behind, on its own\"\n"
             "    keeper color default #FFD166 \"it keeps others, as the hub does\"\n"
             "    on-its-own color default #8BE38B \"it lives on by itself, as the clock does\"\n"
             "    serving color default #8FA3B8 \"it serves others for as long as the machine runs\"\n"
             "    task color default #C8C8C8 \"it ends, and whoever started it waits for it\"\n"
             "journal \"the colors of the journal's lines\"\n"
             "    started color default #8BE38B \"a program started\"\n"
             "    ended color default #8FA3B8 \"a program ended\"\n"
             "    fell color default #FF6B6B \"a program fell\"\n"
             "    raised color default #FFD166 \"a program raised again after a fall\"\n"
             "    board color default #5FD7FF \"what the board itself did\"\n"
             "programs \"the list of programs\"\n"
             "    sort one-of processor memory name age default processor "
             "\"what orders them: the processor they use, their memory, their name, or how long they have lived\"\n"
             "    group one-of living none default living "
             "\"how they are grouped: by where and how each lives — on the screen, behind, keeping others, on its "
             "own, serving — or not at all\"\n"
             "    strands on-or-off default off \"every program's strands shown under it, not only the chosen one's\"\n"
             "panels \"what the board shows beside the programs\"\n"
             "    cores on-or-off default on \"each core — the system's and the programs' — and its load\"\n"
             "    machine on-or-off default on \"memory, the volume and the Use Context\"\n"
             "    journal on-or-off default on \"what happened: programs started, ended, fallen and raised again\"\n"
             "keys \"the board's keys\"\n"
             "    end key default ctrl+x "
             "\"end the chosen program: pressed once it is asked to finish, twice it ends\"\n"
             "    sort key default ctrl+s \"order the programs the next way\"\n"
             "    group key default ctrl+g \"group the programs, or not\"\n"
             "    help key default ctrl+? \"every key of the board\"\n");

namespace taskboard {

namespace {

SortBy SortOf(const std::string &Word)
{
    return Word == "memory" ? SortBy::Memory : Word == "name" ? SortBy::Name : Word == "age" ? SortBy::Age
                                                                                             : SortBy::Processor;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.RefreshMs      = static_cast<std::uint32_t>(settings::number("refresh")),
                .HistorySeconds = static_cast<std::uint32_t>(settings::number("history")),
                .Cool           = Color::FromRaw(settings::color("load cool")),
                .Warm           = Color::FromRaw(settings::color("load warm")),
                .Hot            = Color::FromRaw(settings::color("load hot")),
                .Page           = Color::FromRaw(settings::color("colors page")),
                .Panel          = Color::FromRaw(settings::color("colors panel")),
                .Bars           = Color::FromRaw(settings::color("colors bars")),
                .Frame          = Color::FromRaw(settings::color("colors frame")),
                .Title          = Color::FromRaw(settings::color("colors title")),
                .Text           = Color::FromRaw(settings::color("colors text")),
                .Quiet          = Color::FromRaw(settings::color("colors quiet")),
                .Chosen         = Color::FromRaw(settings::color("colors chosen")),
                .Marked         = Color::FromRaw(settings::color("colors marked")),
                .OnScreen       = Color::FromRaw(settings::color("places on-screen")),
                .Behind         = Color::FromRaw(settings::color("places behind")),
                .Keeper         = Color::FromRaw(settings::color("places keeper")),
                .OnItsOwn       = Color::FromRaw(settings::color("places on-its-own")),
                .Serving        = Color::FromRaw(settings::color("places serving")),
                .Task           = Color::FromRaw(settings::color("places task")),
                .Started        = Color::FromRaw(settings::color("journal started")),
                .Ended          = Color::FromRaw(settings::color("journal ended")),
                .Fell           = Color::FromRaw(settings::color("journal fell")),
                .Raised         = Color::FromRaw(settings::color("journal raised")),
                .Itself         = Color::FromRaw(settings::color("journal board")),
                .Sort           = SortOf(settings::choice("programs sort")),
                .Group          = settings::choice("programs group") == "none" ? GroupBy::None : GroupBy::Living,
                .Strands        = settings::on("programs strands"),
                .Cores          = settings::on("panels cores"),
                .Machine        = settings::on("panels machine"),
                .Journal        = settings::on("panels journal"),
                .Keys           = {settings::key("keys end"), settings::key("keys sort"), settings::key("keys group"),
                                   settings::key("keys help")}};
}

Color LoadColor(const Look &Shown, double Load)
{
    const double Clamped = std::clamp(Load, 0.0, 1.0);
    if (Clamped < 0.5) return Color::Mix(Shown.Cool, Shown.Warm, static_cast<std::uint8_t>(Clamped * 2.0 * 255.0));
    return Color::Mix(Shown.Warm, Shown.Hot, static_cast<std::uint8_t>((Clamped - 0.5) * 2.0 * 255.0));
}

}  // namespace taskboard
