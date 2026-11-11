/* look.cpp — the settings, declared and read. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("animation on-or-off default on "
             "\"whether a choice rolls on the glass and settles, or is only said\"\n"
             "length number from 200 to 5000 milliseconds default 1200 \"how long the roll lasts\"\n"
             "range \"the numbers drawn when none are said\"\n"
             "    least number from 0 to 1000000000 default 1 \"the smallest\"\n"
             "    most number from 1 to 1000000000 default 100 \"the largest\"\n"
             "colors \"the colors of the roll\"\n"
             "    rolling color default #7C7C7C \"what is turning, not yet chosen\"\n"
             "    chosen color default #FFC857 \"what it settled on\"\n"
             "    frame color default #464646 \"the brackets round it\"\n");

namespace random_pick {

Look ReadLook()
{
    using box::settings;
    using painter::core::Color;
    return Look{.Animated = settings::on("animation"),
                .RollMs   = static_cast<std::uint32_t>(settings::number("length")),
                .Least    = settings::number("range least"),
                .Most     = settings::number("range most"),
                .Rolling  = Color::FromRaw(settings::color("colors rolling")),
                .Chosen   = Color::FromRaw(settings::color("colors chosen")),
                .Frame    = Color::FromRaw(settings::color("colors frame"))};
}

}  // namespace random_pick
