/* itemcheck from C — painter::gui's C face (painter/gui.h) held from a
 * program's C: an area opened, a group with a heading and a rectangle in it
 * made and set, a text measured, a step shown; the heading changed and the
 * rectangle removed, shown again; the area closed. */

#include "painter/gui.h"

#include "itemcheck_c.h"

/* The display's events until a step is shown: 1, or 0 when a change was
 * refused or no event came to wait for. */
static int Shown(GuiArea *area)
{
    GuiEvent heard;
    for (;;) {
        if (gui_wait(area, &heard, 0) != 0) return 0;
        if (heard.kind == GUI_EVENT_REFUSED) return 0;
        if (heard.kind == GUI_EVENT_SHOWN) return 1;
    }
}

int ItemcheckFromC(float *measured_width, float *measured_height)
{
    GuiArea *area = gui_open(DISPLAY_AREA_OVER, 240, 64, 0);
    if (!area) return 0;
    GuiItem card  = gui_group(area, 0, ITEM_ROLE_PANEL);
    GuiItem title = gui_text(area, card, "from C", ITEM_ROLE_HEADING);
    GuiItem bar   = gui_rectangle(area, card, ITEM_ROLE_NONE);
    int     made  = card != 0 && title != 0 && bar != 0;
    gui_set_place(area, title, 12, 8);
    gui_set_text_size(area, title, 18);
    gui_set_text_weight(area, title, 700);
    gui_set_place(area, bar, 12, 44);
    gui_set_size(area, bar, 120, 6);
    gui_set_fill(area, bar, 0x4E8CFFFFu);
    gui_set_corners(area, bar, 3);
    gui_measure(area, "from C", ITEM_ROLE_HEADING, GUI_BY_CONTENT, measured_width, measured_height);
    int shown = gui_present(area) && Shown(area);
    gui_set_text(area, title, "from C, again");
    gui_remove(area, bar);
    shown = shown && gui_present(area) && Shown(area);
    gui_close(area);
    return made && shown;
}
