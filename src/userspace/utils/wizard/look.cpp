/* look.cpp — the wizard's settings, declared and read. */

#include "look.h"

#include "box/cxx/settings.h"
#include "box/display.h"
#include "box/settings.h"

BOX_SETTINGS("duration number from 3 to 120 seconds default 20 "
             "\"how long a spell lasts; a potion lasts longer the more is in it\"\n"
             "strength number from 10 to 100 percent default 70 "
             "\"how strongly a spell is felt; a potion is felt more the more is in it\"\n"
             "effects \"which effects may be cast, by a spell or a potion; one that is off is left out of it\"\n"
             "    sway on-or-off default on \"rows and columns swing in waves, as in a drunk's eyes\"\n"
             "    mirror on-or-off default on \"the picture turned over, left for right or upside down\"\n"
             "    fade on-or-off default on \"the colors drained towards grey\"\n"
             "    invert on-or-off default on \"the colors turned to their opposites\"\n"
             "    tint on-or-off default on \"the colors seen through colored glass\"\n"
             "    shimmer on-or-off default on \"sparkles on the glass\"\n"
             "colors \"the colors of the tower\"\n"
             "    room color default #1E1B1A \"the wall\"\n"
             "    floor color default #3B322B \"the floor\"\n"
             "    glow color default #B07CFF \"what magic glows in: the staff's stone and its sparks\"\n"
             "    paper color default #E9E2CF \"behind the wizard's words\"\n"
             "    ink color default #24201C \"the wizard's words\"\n"
             "    bars color default #262626 \"the lines at the top and the bottom\"\n"
             "    title color default #F0F0F0 \"the wizard's name and the spell's\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #8A8A8A \"what is said quietly\"\n"
             "keys \"the wizard's keys\"\n"
             "    spell key default ctrl+s \"cast a spell: a charm chosen at random\"\n"
             "    drink key default ctrl+d \"drink what is in the cauldron\"\n"
             "    empty key default ctrl+e \"pour the cauldron out\"\n"
             "    lift key default ctrl+l \"lift the spell or the potion that lies over the glass\"\n"
             "    help key default ctrl+? \"every key of the wizard\"\n");

namespace wizard {

namespace {

// The effects the person allows, as the display's kinds.
std::uint32_t AllowedKinds()
{
    using box::settings;
    std::uint32_t Kinds = 0;
    if (settings::on("effects sway")) Kinds |= DISPLAY_EFFECT_SWAY;
    if (settings::on("effects mirror")) Kinds |= DISPLAY_EFFECT_MIRROR;
    if (settings::on("effects fade")) Kinds |= DISPLAY_EFFECT_FADE;
    if (settings::on("effects invert")) Kinds |= DISPLAY_EFFECT_INVERT;
    if (settings::on("effects tint")) Kinds |= DISPLAY_EFFECT_TINT;
    if (settings::on("effects shimmer")) Kinds |= DISPLAY_EFFECT_SHIMMER;
    return Kinds;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.DurationSeconds = static_cast<std::uint32_t>(settings::number("duration")),
                .StrengthPercent = static_cast<std::uint32_t>(settings::number("strength")),
                .Allowed         = AllowedKinds(),
                .Room            = Color::FromRaw(settings::color("colors room")),
                .Floor           = Color::FromRaw(settings::color("colors floor")),
                .Glow            = Color::FromRaw(settings::color("colors glow")),
                .Paper           = Color::FromRaw(settings::color("colors paper")),
                .Ink             = Color::FromRaw(settings::color("colors ink")),
                .Bars            = Color::FromRaw(settings::color("colors bars")),
                .Title           = Color::FromRaw(settings::color("colors title")),
                .Text            = Color::FromRaw(settings::color("colors text")),
                .Quiet           = Color::FromRaw(settings::color("colors quiet")),
                .Keys            = {settings::key("keys spell"), settings::key("keys drink"),
                                    settings::key("keys empty"), settings::key("keys lift"),
                                    settings::key("keys help")}};
}

}  // namespace wizard
