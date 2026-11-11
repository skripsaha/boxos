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
             "    ripple on-or-off default on \"rings spreading from the middle, the picture bent through them\"\n"
             "    quake on-or-off default on \"the whole picture shaking\"\n"
             "    mosaic on-or-off default on \"the picture coarsened into blocks\"\n"
             "    rainbow on-or-off default on \"the hues turned in waves that run over the glass\"\n"
             "    melt on-or-off default on \"every column sliding down\"\n"
             "    glitch on-or-off default on \"bands of rows torn sideways, their colors pulled apart\"\n"
             "    spotlight on-or-off default on \"only a wandering circle of light is seen\"\n"
             "    sepia on-or-off default on \"an old film: toned brown, flickering\"\n"
             "    snow on-or-off default on \"flakes falling over everything\"\n"
             "    pulse on-or-off default on \"the picture breathing in and out\"\n"
             "talk number from 10 to 120 letters default 45 \"how many letters of his words come out in a second\"\n"
             "critters on-or-off default on \"an owl on the shelf and a cat by the fire\"\n"
             "colors \"the colors of the tower\"\n"
             "    room color default #8A8178 \"the wall's stone, before the fire and the candles light it\"\n"
             "    floor color default #7A5F48 \"the floor's boards\"\n"
             "    glow color default #B07CFF \"what magic glows in: the staff's crystal and its sparks\"\n"
             "    fire color default #FF8A30 \"the fire's light on the walls and on the wizard\"\n"
             "    candle color default #FFC060 \"the candles' light\"\n"
             "    moon color default #A8B8E8 \"the moon's light through the window\"\n"
             "    paper color default #E9E2CF \"behind the wizard's words\"\n"
             "    ink color default #24201C \"the wizard's words\"\n"
             "    bars color default #262626 \"the lines at the top and the bottom\"\n"
             "    title color default #F0F0F0 \"the wizard's name and the spell's\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #8A8A8A \"what is said quietly\"\n"
             "keys \"the wizard's keys\"\n"
             "    spell key default ctrl+s \"cast a spell: a charm chosen at random\"\n"
             "    drink key default ctrl+d \"drink what is in the cauldron, as enter does\"\n"
             "    empty key default ctrl+e \"pour the cauldron out\"\n"
             "    lift key default ctrl+l \"lift the spell or the potion that lies over the glass\"\n"
             "    book key default ctrl+g \"the grimoire: the potions found so far and the spells he knows\"\n"
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
    if (settings::on("effects ripple")) Kinds |= DISPLAY_EFFECT_RIPPLE;
    if (settings::on("effects quake")) Kinds |= DISPLAY_EFFECT_QUAKE;
    if (settings::on("effects mosaic")) Kinds |= DISPLAY_EFFECT_MOSAIC;
    if (settings::on("effects rainbow")) Kinds |= DISPLAY_EFFECT_RAINBOW;
    if (settings::on("effects melt")) Kinds |= DISPLAY_EFFECT_MELT;
    if (settings::on("effects glitch")) Kinds |= DISPLAY_EFFECT_GLITCH;
    if (settings::on("effects spotlight")) Kinds |= DISPLAY_EFFECT_SPOTLIGHT;
    if (settings::on("effects sepia")) Kinds |= DISPLAY_EFFECT_SEPIA;
    if (settings::on("effects snow")) Kinds |= DISPLAY_EFFECT_SNOW;
    if (settings::on("effects pulse")) Kinds |= DISPLAY_EFFECT_PULSE;
    return Kinds;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.DurationSeconds = static_cast<std::uint32_t>(settings::number("duration")),
                .StrengthPercent = static_cast<std::uint32_t>(settings::number("strength")),
                .Allowed         = AllowedKinds(),
                .TalkPerSecond   = static_cast<std::uint32_t>(settings::number("talk")),
                .Critters        = settings::on("critters"),
                .Room            = Color::FromRaw(settings::color("colors room")),
                .Floor           = Color::FromRaw(settings::color("colors floor")),
                .Glow            = Color::FromRaw(settings::color("colors glow")),
                .Fire            = Color::FromRaw(settings::color("colors fire")),
                .Candle          = Color::FromRaw(settings::color("colors candle")),
                .Moon            = Color::FromRaw(settings::color("colors moon")),
                .Paper           = Color::FromRaw(settings::color("colors paper")),
                .Ink             = Color::FromRaw(settings::color("colors ink")),
                .Bars            = Color::FromRaw(settings::color("colors bars")),
                .Title           = Color::FromRaw(settings::color("colors title")),
                .Text            = Color::FromRaw(settings::color("colors text")),
                .Quiet           = Color::FromRaw(settings::color("colors quiet")),
                .Keys            = {settings::key("keys spell"), settings::key("keys drink"),
                                    settings::key("keys empty"), settings::key("keys lift"),
                                    settings::key("keys book"), settings::key("keys help")}};
}

}  // namespace wizard
