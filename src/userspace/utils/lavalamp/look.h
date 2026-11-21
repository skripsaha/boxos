/* look.h — what the person set of the lamp (settings lavalamp): which way it
 * is drawn and the lamp it begins as. Read whole, and read again when the
 * file is saved or the Use Context changes; the lamp on the screen then takes
 * it, whatever the panel had made of it. The program never writes it. */
#ifndef LAVALAMP_LOOK_H
#define LAVALAMP_LOOK_H

#include "composition.h"

namespace lavalamp {

struct Look {
    bool        Cells = false;   // painter::tui's cells rather than the screen's pixels
    Composition Lamp;
};

Look ReadLook();

}  // namespace lavalamp

#endif
