/* look.cpp — the sea battle's settings, declared and read. */

#include "look.h"

#include "box/cxx/settings.h"
#include "box/settings.h"

BOX_SETTINGS("players \"the names a new game begins with; they are typed again on the setup screen\"\n"
             "    first text default \"Captain 1\" \"the first player\"\n"
             "    second text default \"Captain 2\" \"the second player\"\n"
             "game \"how a new game begins; the setup screen changes each of these for the game being set up\"\n"
             "    grid one-of 6x6 8x8 10x10 12x12 default 10x10 \"the size of each player's waters\"\n"
             "    fleet one-of classic skirmish armada duel default classic "
             "\"the ships: classic is 1x4 2x3 3x2 4x1, skirmish 1x3 2x2 2x1, armada 1x5 2x4 2x3 3x2 3x1, duel 1x4 1x3 1x2\"\n"
             "    touching on-or-off default off \"ships may lie side by side and corner to corner; the classic rule is that they may not\"\n"
             "    extra-shot on-or-off default on \"a hit earns another shot\"\n"
             "    salvo on-or-off default off \"as many shots in a turn as ships afloat; a hit earns no extra shot\"\n"
             "    speed one-of slow normal fast instant default normal "
             "\"how fast the shells fly and the ships sink; instant shows only where each shot fell\"\n"
             "colors \"the colors of the sea and the ships\"\n"
             "    sea color default #1F6FA6 \"the water; its depth is shaded from it\"\n"
             "    ships color default #8D99A6 \"the ships' steel\"\n"
             "    hits color default #FF8A2B \"fire where a ship is hit\"\n"
             "    misses color default #DDF3FF \"the splash and ripples where a shell fell in the water\"\n"
             "    bars color default #0B1A2C \"the lines at the top and the bottom, and what frames the waters\"\n"
             "    title color default #F2C14E \"titles, the cursor and the names\"\n"
             "    text color default #D6E2EE \"what is said\"\n"
             "    quiet color default #7E92A6 \"what is said quietly\"\n"
             "    good color default #7BE08F \"a place where a ship may be laid, a win\"\n"
             "    bad color default #FF6B6B \"a place where it may not, a loss\"\n"
             "keys \"the keys of the sea battle\"\n"
             "    rotate key default ctrl+r \"turn the ship being placed, as r and tab do\"\n"
             "    auto key default ctrl+a \"lay the fleet at random, as a does\"\n"
             "    undo key default ctrl+z \"take the last ship back, as u and backspace do\"\n"
             "    clear key default ctrl+x \"take every ship back, as c does\"\n"
             "    help key default ctrl+? \"every key of the sea battle\"\n");

namespace seabattle {

namespace {

int GridOf(const std::string &Word)
{
    if (Word == "6x6") return 6;
    if (Word == "8x8") return 8;
    if (Word == "12x12") return 12;
    return 10;
}

FleetKind FleetOf(const std::string &Word)
{
    if (Word == "skirmish") return FleetKind::Skirmish;
    if (Word == "armada") return FleetKind::Armada;
    if (Word == "duel") return FleetKind::Duel;
    return FleetKind::Classic;
}

Pace PaceOf(const std::string &Word)
{
    if (Word == "slow") return Pace::Slow;
    if (Word == "fast") return Pace::Fast;
    if (Word == "instant") return Pace::Instant;
    return Pace::Normal;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.NameOne = settings::text("players first"),
                .NameTwo = settings::text("players second"),
                .Start = Rules{.Grid = GridOf(settings::choice("game grid")),
                               .Fleet = FleetOf(settings::choice("game fleet")),
                               .Touching = settings::on("game touching"),
                               .ExtraShot = settings::on("game extra-shot"),
                               .Salvo = settings::on("game salvo")},
                .Speed = PaceOf(settings::choice("game speed")),
                .Sea = Color::FromRaw(settings::color("colors sea")),
                .Ship = Color::FromRaw(settings::color("colors ships")),
                .Fire = Color::FromRaw(settings::color("colors hits")),
                .Splash = Color::FromRaw(settings::color("colors misses")),
                .Bars = Color::FromRaw(settings::color("colors bars")),
                .Title = Color::FromRaw(settings::color("colors title")),
                .Text = Color::FromRaw(settings::color("colors text")),
                .Quiet = Color::FromRaw(settings::color("colors quiet")),
                .Good = Color::FromRaw(settings::color("colors good")),
                .Bad = Color::FromRaw(settings::color("colors bad")),
                .Keys = {settings::key("keys rotate"), settings::key("keys auto"), settings::key("keys undo"),
                         settings::key("keys clear"), settings::key("keys help")}};
}

Look Adapted(Look Wanted, bool Text)
{
    if (!Text) return Wanted;
    Wanted.Sea = Rgb(0x00, 0x00, 0xAA);
    Wanted.Ship = Rgb(0xAA, 0xAA, 0xAA);
    Wanted.Fire = Rgb(0xFF, 0x55, 0x55);
    Wanted.Splash = Rgb(0xFF, 0xFF, 0xFF);
    Wanted.Bars = Rgb(0x00, 0x00, 0x00);
    return Wanted;
}

std::uint32_t Paced(Pace Speed, std::uint32_t Ms)
{
    switch (Speed) {
    case Pace::Slow:    return Ms * 3 / 2;
    case Pace::Fast:    return Ms * 11 / 20;
    case Pace::Instant: return 0;
    case Pace::Normal:  break;
    }
    return Ms;
}

}  // namespace seabattle
