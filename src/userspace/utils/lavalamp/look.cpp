/* look.cpp — the lamp's settings, declared, read and written. See look.h. */

#include "look.h"

#include <string>

#include "box/core/cabin.h"
#include "box/cxx/error.h"
#include "box/cxx/settings.h"
#include "box/error.h"
#include "box/settings.h"

BOX_SETTINGS("tui on-or-off default off "
             "\"the lamp drawn in the console's cells (painter::tui) rather than in the screen's own pixels\"\n"
             "remember on-or-off default off "
             "\"what the lamp's panel changes written here, so the lamp begins so the next time\"\n"
             "palette one-of lava magma ocean lime violet sunset own default lava "
             "\"the lamp's colors: one of the palettes, or own — the three colors below\"\n"
             "colors \"the lamp's own colors, when the palette is own\"\n"
             "    wax color default #FF3A6B \"the wax where it is hottest\"\n"
             "    rim color default #7A1040 \"the wax where it cools: toward its edge and up in the lamp\"\n"
             "    glass color default #100820 \"the liquid the wax floats in\"\n"
             "blobs number from 2 to 24 default 7 \"how many blobs of wax\"\n"
             "size number from 50 to 200 percent default 100 \"how large the blobs are\"\n"
             "flow number from 1 to 9 default 5 \"how fast the wax rises and sinks\"\n"
             "glow number from 0 to 100 percent default 60 \"how far the wax lights the liquid round it\"\n"
             "light on-or-off default on "
             "\"the wax lit as a round thing is — shaded, its highlights, the bulb's light from below; off, flat colors\"\n");

namespace lavalamp {

namespace {

std::string OnOff(bool On)
{
    return On ? "on" : "off";
}

/* The program's own settings as the utility settings holds them, to be
 * written: read once for a change, given back after it. */
class Pen {
public:
    Pen() : Code(settings_read(cabin_info()->program_file, &Held)) {}
    ~Pen()
    {
        if (Code == 0) settings_release(&Held);
    }
    Pen(const Pen &)            = delete;
    Pen &operator=(const Pen &) = delete;

    /* False, and Refused said, when the setting was not written — nor will
     * any after it be. */
    bool Set(const char *Path, const std::string &Value)
    {
        if (Refused) return false;
        if (Code != 0) {
            Refused = "the settings could not be read: " + std::string(box::error(static_cast<::error_t>(-Code)).message());
            return false;
        }
        const char *Why = nullptr;
        if (const int Wrote = settings_set(&Held, nullptr, Path, Value.c_str(), &Why); Wrote != 0) {
            Refused = Wrote == -ERR_INVALID_ARGUMENT && Why ? std::string(Why)
                                                            : "the settings file would not be saved: " +
                                                                  std::string(box::error(static_cast<::error_t>(-Wrote)).message());
            return false;
        }
        return true;
    }

    std::optional<std::string> Refused;

private:
    BoxSettings Held{};
    int         Code;
};

}  // namespace

Look ReadLook()
{
    using box::settings;
    Look Read;
    Read.Cells    = settings::on("tui");
    Read.Remember = settings::on("remember");
    if (const std::optional<int> Which = PaletteNamed(settings::choice("palette"))) Read.Lamp.Take(*Which);
    else {
        Read.Lamp.Palette = Own;
        Read.Lamp.Wax     = Color::FromRaw(settings::color("colors wax"));
        Read.Lamp.Rim     = Color::FromRaw(settings::color("colors rim"));
        Read.Lamp.Glass   = Color::FromRaw(settings::color("colors glass"));
    }
    Read.Lamp.Blobs = static_cast<int>(settings::number("blobs"));
    Read.Lamp.Size  = static_cast<int>(settings::number("size"));
    Read.Lamp.Flow  = static_cast<int>(settings::number("flow"));
    Read.Lamp.Glow  = static_cast<int>(settings::number("glow"));
    Read.Lamp.Light = settings::on("light");
    return Read;
}

std::optional<std::string> WriteLook(const Look &Wanted, Look &Settled)
{
    if (Wanted == Settled) return std::nullopt;
    Pen         Write;
    const auto  Number = [&](const char *Path, int Is, int &Was) {
        if (Is != Was && Write.Set(Path, std::to_string(Is))) Was = Is;
    };
    const auto  Switch = [&](const char *Path, bool Is, bool &Was) {
        if (Is != Was && Write.Set(Path, OnOff(Is))) Was = Is;
    };
    const Composition &Is  = Wanted.Lamp;
    Composition       &Was = Settled.Lamp;
    Switch("remember", Wanted.Remember, Settled.Remember);
    Switch("tui", Wanted.Cells, Settled.Cells);
    /* A lamp of its own colours writes all three: what the file holds for
     * them while a palette is chosen is not known here. */
    if (Is.Palette != Was.Palette || (Is.Palette == Own && !(Is.Wax == Was.Wax && Is.Rim == Was.Rim && Is.Glass == Was.Glass))) {
        const bool Colours = Is.Palette != Own || (Write.Set("colors wax", Spelled(Is.Wax)) && Write.Set("colors rim", Spelled(Is.Rim)) &&
                                                   Write.Set("colors glass", Spelled(Is.Glass)));
        if (Colours && Write.Set("palette", Is.PaletteName())) {
            Was.Palette = Is.Palette;
            Was.Wax     = Is.Wax;
            Was.Rim     = Is.Rim;
            Was.Glass   = Is.Glass;
        }
    }
    Number("blobs", Is.Blobs, Was.Blobs);
    Number("size", Is.Size, Was.Size);
    Number("flow", Is.Flow, Was.Flow);
    Number("glow", Is.Glow, Was.Glow);
    Switch("light", Is.Light, Was.Light);
    return Write.Refused;
}

}  // namespace lavalamp
