/* look.h — what the person set of the lamp (settings lavalamp,
 * lavalamp.settings): which way it is drawn, whether the panel's changes are
 * remembered, and the lamp it begins as. Read whole, and read again when the
 * file is saved or the Use Context changes. Written only at the person's
 * word in the panel: while remember is on, what the panel changes; and
 * remember itself. */
#ifndef LAVALAMP_LOOK_H
#define LAVALAMP_LOOK_H

#include <optional>
#include <string>

#include "composition.h"

namespace lavalamp {

struct Look {
    bool        Cells    = false;   // painter::tui's cells rather than the screen's pixels
    bool        Remember = false;   // the panel's changes written to the settings
    Composition Lamp;
    bool operator==(const Look &) const = default;
};

Look ReadLook();

/* The settings made to say what Wanted says, a setting at a time, only those
 * that differ from Settled — what the file says now — and Settled kept up
 * with each one written. The colours go with the palette: a lamp of its own
 * colours writes all three. A sentence when the file would not take one. */
std::optional<std::string> WriteLook(const Look &Wanted, Look &Settled);

}  // namespace lavalamp

#endif
