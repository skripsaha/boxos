/* look.cpp — the sandbox's settings, declared and read. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("speed number from 10 to 240 steps default 60 \"how many steps the sand takes in a second\"\n"
             "flow number from 1 to 8 cells default 3 \"how far water runs sideways in a step\"\n"
             "gravity one-of down up default down \"which way things fall\"\n"
             "brush number from 1 to 16 pixels default 3 \"the brush's size when the sandbox opens\"\n"
             "variety number from 0 to 100 percent default 30 "
             "\"how much each grain differs in color from the next: 0 paints every grain alike\"\n"
             "colors \"the colors of what is in the sandbox\"\n"
             "    sand color default #E8C468 \"a grain of sand\"\n"
             "    water color default #2D7DD2 \"water\"\n"
             "    oil color default #8A5A2B \"oil, which floats on water and burns hot\"\n"
             "    stone color default #7A7A86 \"stone, which stays\"\n"
             "    wood color default #A0622D \"wood, which burns\"\n"
             "    plant color default #3DAA4A \"a plant, which grows on the water it drinks\"\n"
             "    seed color default #E3D3A8 \"a seed, which takes root beside water\"\n"
             "    fire color default #FF6A13 \"a flame\"\n"
             "    smoke color default #8C8C96 \"smoke, which thins as it rises\"\n"
             "    steam color default #D6DCE8 \"steam, which falls as water once it cools\"\n"
             "    lava color default #E03A0E \"lava, which melts sand to glass and boils water\"\n"
             "    glass color default #9FE3EE \"glass\"\n"
             "    page color default #0C0C0C \"behind it all: the empty air\"\n"
             "    cursor color default white \"the ring of the brush\"\n"
             "keys \"the sandbox's keys\"\n"
             "    help key default ctrl+? \"every key of the sandbox\"\n"
             "    faster key default ctrl+right \"more steps in a second\"\n"
             "    slower key default ctrl+left \"fewer steps in a second\"\n"
             "    gravity key default ctrl+g \"turn gravity over\"\n");

namespace sand {

namespace {

// The setting that holds each kind's color; the air has none.
constexpr const char *ColorPaths[KindCount] = {
    nullptr,       "colors sand",  "colors water", "colors oil",  "colors stone", "colors wood", "colors plant",
    "colors seed", "colors fire",  "colors smoke", "colors steam", "colors lava",  "colors glass"};

}  // namespace

Look ReadLook()
{
    using box::settings;
    Look Read{.Speed     = static_cast<std::uint32_t>(settings::number("speed")),
              .Flow      = static_cast<int>(settings::number("flow")),
              .GravityUp = settings::choice("gravity") == "up",
              .Brush     = static_cast<int>(settings::number("brush")),
              .Variety   = static_cast<int>(settings::number("variety")),
              .Colors    = {},
              .Cursor    = Color::FromRaw(settings::color("colors cursor")),
              .Keys      = {settings::key("keys help"), settings::key("keys faster"), settings::key("keys slower"),
                            settings::key("keys gravity")}};
    for (int Which = 1; Which < KindCount; Which++) Read.Colors[Which] = Color::FromRaw(settings::color(ColorPaths[Which]));
    Read.Colors[static_cast<int>(Kind::Air)] = Color::FromRaw(settings::color("colors page"));
    return Read;
}

}  // namespace sand
