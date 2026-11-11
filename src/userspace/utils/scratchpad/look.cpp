/* look.cpp — the scratchpad's settings, declared and read; and the choice of a
 * random paper. */

#include "look.h"

#include <random>

#include "box/cxx/settings.h"
#include "box/settings.h"

BOX_SETTINGS("new \"what a new note is given\"\n"
             "    random on-or-off default on "
             "\"a new note takes a paper from the palette, and an ink that reads on it; off, it takes the paper and "
             "text below\"\n"
             "    paper color default #FFE680 \"the paper of a new note when random is off\"\n"
             "    text color default #1A1A1A \"the ink of a new note when random is off\"\n"
             "palette \"the papers a random note may take, and a note given a new color by its key\"\n"
             "    yellow color default #FFE680 \"a yellow paper\"\n"
             "    pink color default #FFB3C7 \"a pink paper\"\n"
             "    mint color default #A8E6CF \"a mint paper\"\n"
             "    sky color default #9AD1FF \"a sky paper\"\n"
             "    lilac color default #CDB4FF \"a lilac paper\"\n"
             "    peach color default #FFC999 \"a peach paper\"\n"
             "    lime color default #D4EE8A \"a lime paper\"\n"
             "    sand color default #E8D5B0 \"a sand paper\"\n"
             "window \"the size of the note, in cells of the glass; a smaller glass takes less\"\n"
             "    width number from 24 to 100 columns default 52 \"how wide\"\n"
             "    height number from 6 to 40 rows default 14 \"how high\"\n"
             "keys \"the scratchpad's keys\"\n"
             "    color key default ctrl+r \"give the note another random paper\"\n"
             "    trash key default ctrl+d \"move the note to the trash, once asked\"\n"
             "    all key default ctrl+o \"page through every note there is\"\n"
             "    help key default ctrl+? \"every key of the scratchpad\"\n");

namespace scratchpad {

namespace {

// From this brightness (0..255) a paper is light, and its ink dark. The ink is
// a pure black or white: a tint would fold into another color on a screen of
// sixteen.
constexpr unsigned LightFrom = 140;

Color InkOn(Color Paper)
{
    const unsigned Brightness = (299u * Paper.Red() + 587u * Paper.Green() + 114u * Paper.Blue()) / 1000u;
    return Brightness >= LightFrom ? painter::core::Colors::Black : painter::core::Colors::White;
}

std::size_t Draw(std::size_t Count)
{
    static std::random_device Device;
    return Device() % Count;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    static constexpr const char *Papers[PaletteSize] = {"palette yellow", "palette pink",  "palette mint",
                                                        "palette sky",    "palette lilac", "palette peach",
                                                        "palette lime",   "palette sand"};
    Look Read{.Random  = settings::on("new random"),
              .Fixed   = {Color::FromRaw(settings::color("new paper")), Color::FromRaw(settings::color("new text"))},
              .Palette = {},
              .Width   = static_cast<std::int32_t>(settings::number("window width")),
              .Height  = static_cast<std::int32_t>(settings::number("window height")),
              .Keys    = {settings::key("keys color"), settings::key("keys trash"), settings::key("keys all"),
                          settings::key("keys help")}};
    for (std::size_t At = 0; At < PaletteSize; At++) Read.Palette[At] = Color::FromRaw(settings::color(Papers[At]));
    return Read;
}

Sheet NewSheet(const Look &Shown)
{
    if (!Shown.Random) return Shown.Fixed;
    const Color Paper = Shown.Palette[Draw(PaletteSize)];
    return {Paper, InkOn(Paper)};
}

Sheet OtherSheet(const Look &Shown, Sheet Now)
{
    std::size_t Others[PaletteSize];
    std::size_t Count = 0;
    for (std::size_t At = 0; At < PaletteSize; At++)
        if (Shown.Palette[At] != Now.Paper) Others[Count++] = At;
    if (Count == 0) return Now;
    const Color Paper = Shown.Palette[Others[Draw(Count)]];
    return {Paper, InkOn(Paper)};
}

}  // namespace scratchpad
