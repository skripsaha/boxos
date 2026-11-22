/* setup.h — the lamp as it is now and the settings as they are, shared by
 * both faces and kept across a change of face: the keys go through the
 * panel's tuning to the lamp, and while remember is on what they change is
 * written to the settings at once; a settings file saved from elsewhere, or a
 * change of the Use Context, gives the lamp what it says. */
#ifndef LAVALAMP_SETUP_H
#define LAVALAMP_SETUP_H

#include "box/cxx/settings.h"
#include "painter/tui/key.h"

#include "look.h"
#include "tuning.h"

namespace lavalamp {

class Setup {
public:
    /* Begin: the lamp to light — the settings', or a kept lamp's, or the
     * line's — and Settings: what the settings say. */
    Setup(const Look &Begin, const Look &Settings);

    const Look   &Now() const noexcept { return Current; }
    const Tuning &Panel() const noexcept { return Tuned; }
    Tuning       &Panel() noexcept { return Tuned; }

    Heard Key(const painter::tui::KeyEvent &Key);
    /* Heard::Lamp or Heard::Face when the settings were saved or the Use
     * Context changed and they say other than they did. */
    Heard Reread();

private:
    void Write(const Look &Wanted);

    Look                 Current, Settled;
    Tuning               Tuned;
    box::settings::watch Watch;
};

}  // namespace lavalamp

#endif
