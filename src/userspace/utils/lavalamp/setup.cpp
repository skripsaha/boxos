/* setup.cpp — the lamp now, the settings, and the one written to the other.
 * See setup.h. */

#include "setup.h"

namespace lavalamp {

Setup::Setup(const Look &Begin, const Look &Settings) : Current(Begin), Settled(Settings) {}

Heard Setup::Key(const painter::tui::KeyEvent &Key)
{
    const Heard What = Tuned.Key(Key, Current);
    switch (What) {
    case Heard::Lamp:
    case Heard::Face:
        if (Current.Remember) Write(Current);
        break;
    case Heard::Remember:
        if (Current.Remember) {
            Tuned.Say("kept in lavalamp.settings from now on");
            Write(Current);
        } else {
            /* Only remember itself: the settings keep the lamp they had. */
            Look Forget     = Settled;
            Forget.Remember = false;
            Tuned.Say("changes last until you leave");
            Write(Forget);
        }
        break;
    default: break;
    }
    return What;
}

void Setup::Write(const Look &Wanted)
{
    if (const std::optional<std::string> Refused = WriteLook(Wanted, Settled)) Tuned.Say("not kept: " + *Refused);
}

Heard Setup::Reread()
{
    if (!Watch.changed()) return Heard::Nothing;
    const Look Read = ReadLook();
    /* The lamp's own writing comes back as what it wrote. */
    if (Read == Settled) return Heard::Nothing;
    const bool Face = Read.Cells != Current.Cells;
    Settled         = Read;
    Current         = Read;
    return Face ? Heard::Face : Heard::Lamp;
}

}  // namespace lavalamp
