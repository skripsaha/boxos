/* look.cpp — the lamp's settings, declared and read. */

#include "look.h"

#include <string>

#include "box/cxx/settings.h"
#include "box/settings.h"

BOX_SETTINGS("tui on-or-off default off next-start "
             "\"the lamp drawn in the console's cells (painter::tui) rather than in the screen's own pixels\"\n"
             "palette one-of lava magma ocean lime violet sunset own default lava "
             "\"the lamp's colors: one of the palettes, or own — the three colors below\"\n"
             "colors \"the lamp's own colors, when the palette is own\"\n"
             "    wax color default #FF3A6B \"the wax where it is hottest\"\n"
             "    rim color default #7A1040 \"the wax where it cools: toward its edge and up in the lamp\"\n"
             "    glass color default #100820 \"the liquid the wax floats in\"\n"
             "blobs number from 2 to 24 default 7 \"how many blobs of wax\"\n"
             "size number from 50 to 200 percent default 100 \"how large the blobs are\"\n"
             "flow number from 1 to 9 default 5 \"how fast the wax rises and sinks\"\n"
             "glow number from 0 to 100 percent default 60 \"how far the wax lights the liquid round it\"\n");

namespace lavalamp {

Look ReadLook()
{
    using box::settings;
    Look Read;
    Read.Cells = settings::on("tui");
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
    return Read;
}

}  // namespace lavalamp
