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
             "    help key default ctrl+h \"every key of the board\"\n");

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
