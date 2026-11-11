// panel.h — the words over the sky: the line at the bottom, and the window of
// keys.
#ifndef ORBITS_PANEL_H
#define ORBITS_PANEL_H

#include <cstddef>
#include <string>

#include "painter/tui/frame.h"

#include "look.h"

namespace orbits {

// What the line at the bottom says.
struct Readout {
    const char *Preset   = "";
    std::size_t Bodies   = 0;
    std::size_t Merged   = 0;
    std::size_t Escaped  = 0;
    double      Speed    = 1.0;
    bool        Paused   = false;
    double      Mass     = 1.0;
    double      Power    = 0.0;
    int         Degrees  = 0;
    bool        KeysFirst = false;   // the keys in place of the numbers
    std::string Note;                // what has just happened, in place of either
    std::string Spelled[KeyCount];   // the keys a person chose, as they spell them
};

void PaintBar(painter::tui::Frame &Out, const Readout &Said);
void PaintHelp(painter::tui::Frame &Out, const Readout &Said);

}  // namespace orbits

#endif  // ORBITS_PANEL_H
