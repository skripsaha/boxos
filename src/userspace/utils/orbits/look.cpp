// look.cpp — the sky's settings, declared and read.

#include "look.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "box/cxx/settings.h"
#include "box/settings.h"

#include "presets.h"

BOX_SETTINGS("preset one-of solar binary eight chaos lone default solar "
             "\"the sky it begins with: a calm solar system, a binary star, three suns in a figure-eight, a star and a "
             "crowd on crossing orbits, or one star alone\"\n"
             "trail number from 1 to 60 seconds default 6 \"how long a body's trail takes to fade\"\n"
             "gravity number from 10 to 400 percent default 100 "
             "\"how strongly bodies pull each other; the presets are set up for 100, and orbits go wide or fall in "
             "when it is changed\"\n"
             "speed number from 10 to 800 percent default 100 \"how fast time runs when it begins\"\n"
             "colors \"the colors of the sky\"\n"
             "    palette one-of spectrum ember ocean aurora mono default spectrum \"the colors the bodies take\"\n"
             "    star color default #FFD27A \"the color of a star\"\n"
             "    background color default #050505 \"the night behind\"\n"
             "aim \"the aim of the next throw\"\n"
             "    show on-or-off default on \"the dotted path the next throw would take\"\n"
             "    color color default #E8E8E8 \"the color of the aim\"\n"
             "keys \"the sky's keys\"\n"
             "    help key default ctrl+? \"every key of the sky\"\n"
             "    clear key default ctrl+l \"clear every trail\"\n"
             "    reset key default ctrl+r \"begin the sky again\"\n"
             "    aim key default ctrl+a \"show the aim, or hide it\"\n");

namespace orbits {

namespace {

constexpr std::size_t RampStops = 5;

struct Ramp {
    Color Stops[RampStops];
};

const Ramp &RampOf(Palette Which)
{
    static const Ramp All[] = {
        {{Color::Rgb(0xFF, 0x5A, 0x5A), Color::Rgb(0xFF, 0xD0, 0x4A), Color::Rgb(0x5A, 0xF0, 0x7A),
          Color::Rgb(0x4A, 0xC8, 0xFF), Color::Rgb(0xD0, 0x6A, 0xFF)}},
        {{Color::Rgb(0xFF, 0x3A, 0x1E), Color::Rgb(0xFF, 0x7A, 0x1A), Color::Rgb(0xFF, 0xB0, 0x30),
          Color::Rgb(0xFF, 0xDC, 0x6A), Color::Rgb(0xFF, 0xF2, 0xB0)}},
        {{Color::Rgb(0x2A, 0x7A, 0xFF), Color::Rgb(0x2A, 0xB8, 0xFF), Color::Rgb(0x4A, 0xE8, 0xE0),
          Color::Rgb(0x8A, 0xF8, 0xC8), Color::Rgb(0xE0, 0xFF, 0xFF)}},
        {{Color::Rgb(0x3A, 0xFF, 0x8A), Color::Rgb(0x3A, 0xE8, 0xD8), Color::Rgb(0x5A, 0x9A, 0xFF),
          Color::Rgb(0xA0, 0x6A, 0xFF), Color::Rgb(0xFF, 0x7A, 0xD0)}},
        {{Color::Rgb(0x90, 0x98, 0xA8), Color::Rgb(0xB0, 0xB8, 0xC8), Color::Rgb(0xD0, 0xD8, 0xE4),
          Color::Rgb(0xE8, 0xEE, 0xF6), Color::Rgb(0xFF, 0xFF, 0xFF)}},
    };
    return All[static_cast<std::size_t>(Which)];
}

Palette PaletteOf(const std::string &Word)
{
    return Word == "ember" ? Palette::Ember : Word == "ocean" ? Palette::Ocean : Word == "aurora" ? Palette::Aurora
         : Word == "mono"  ? Palette::Mono  : Palette::Spectrum;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    const std::size_t Preset = PresetNamed(settings::choice("preset").c_str());
    return Look{.Preset         = Preset == PresetCount ? 0 : Preset,
                .TrailSeconds   = static_cast<std::uint32_t>(settings::number("trail")),
                .GravityPercent = static_cast<std::uint32_t>(settings::number("gravity")),
                .SpeedPercent   = static_cast<std::uint32_t>(settings::number("speed")),
                .Colors         = PaletteOf(settings::choice("colors palette")),
                .Star           = Color::FromRaw(settings::color("colors star")),
                .Background     = Color::FromRaw(settings::color("colors background")),
                .ShowAim        = settings::on("aim show"),
                .Aim            = Color::FromRaw(settings::color("aim color")),
                .Keys           = {settings::key("keys help"), settings::key("keys clear"), settings::key("keys reset"),
                                   settings::key("keys aim")}};
}

Color BodyColor(const Look &Shown, double Tone)
{
    const Ramp        &Stops  = RampOf(Shown.Colors);
    const double       Place  = std::clamp(Tone, 0.0, 1.0) * (RampStops - 1);
    const std::size_t  Low    = std::min(static_cast<std::size_t>(Place), RampStops - 2);
    const double       Part   = Place - static_cast<double>(Low);
    return Color::Mix(Stops.Stops[Low], Stops.Stops[Low + 1], static_cast<std::uint8_t>(Part * 255.0));
}

Color StarColor(const Look &Shown, double Tone)
{
    return Color::Mix(Shown.Star, BodyColor(Shown, Tone), static_cast<std::uint8_t>(std::clamp(Tone, 0.0, 1.0) * 200.0));
}

}  // namespace orbits
