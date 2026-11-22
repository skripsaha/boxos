/* panel.h — the tuning panel and the line of keys over the lamp in pixels, as
 * items (painter::gui): a pane of the theme's panel the lamp glows through, a
 * row for each thing the lamp is made of — a colour's swatch, a number's bar,
 * the palette's three colours, a switch — the chosen row marked by the theme's
 * selection gliding to it, and what the row says of the last key. What the
 * panel does is tuning.h's; this only shows it. */
#ifndef LAVALAMP_PANEL_H
#define LAVALAMP_PANEL_H

#include <array>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "look.h"
#include "tuning.h"

namespace lavalamp {

namespace gui = painter::gui;

class Panel {
public:
    explicit Panel(gui::Area &Screen);

    /* The panel and the line of keys as the tuning and the lamp are now. */
    void Show(const Tuning &State, const Look &Now);
    /* The screen changed size: the panel laid where it goes now. */
    void Place();

private:
    struct Line {
        gui::Text  Name, Value;
        gui::Shape Swatch, Track, Bar;
        bool       On = false;   // a switch, as last shown
    };

    void ShowLine(int At, const Tuning &State, const Look &Now);

    gui::Area              &Screen;
    gui::Group              Whole;
    gui::Shape              Plate, Chosen;
    gui::Text               Title, Note;
    std::array<Line, Rows>  Lines;
    gui::Text               Keys;
    bool                    Opened  = false;
    bool                    Started = false;
    Row                     Marked  = Row::Palette;
};

}  // namespace lavalamp

#endif
