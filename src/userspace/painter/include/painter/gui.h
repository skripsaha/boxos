#ifndef PAINTER_GUI_C_H
#define PAINTER_GUI_C_H

/*
 * painter::gui for C — the same areas and items (painter/gui/area.h), named
 * plainly, for a program written in C:
 *
 *     GuiArea *screen = gui_open(DISPLAY_AREA_OVER, 200, 40, 0);
 *     GuiItem  time   = gui_text(screen, 0, "12:46", ITEM_ROLE_HEADING);
 *     gui_set_place(screen, time, 16, 8);
 *     gui_present(screen);
 *     gui_set_text(screen, time, "12:47");    // only this goes
 *     gui_present(screen);
 *     gui_close(screen);
 *
 * An item is a number, 0 for none; made under a group, or under the area
 * itself (0), it stays until gui_remove — a group's items with it — or until
 * the area closes. A property set again as it was goes nowhere. Places and
 * sizes are in points; GUI_BY_CONTENT lets the content decide a size; colours
 * are 0xRRGGBBAA. Placements are box/items.h DISPLAY_AREA_*, roles
 * ITEM_ROLE_*.
 */

#include "box/items.h"
#include "box/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GuiArea GuiArea;
typedef uint32_t       GuiItem;

#define GUI_BY_CONTENT (-1.0f)

/* What gui_wait heard. */
enum {
    GUI_EVENT_AREA      = 0,   /* the area's size, scale or glass now, or whether it is seen */
    GUI_EVENT_SIZE      = 1,   /* an item sized by its content came to width x height */
    GUI_EVENT_REFUSED   = 2,   /* a change the display did not take */
    GUI_EVENT_SHOWN     = 3,   /* a step on the glass */
    GUI_EVENT_PAGE_FREE = 4,   /* a canvas's page free again */
    GUI_EVENT_KEY       = 5,   /* a key: character, scancode, mods */
    GUI_EVENT_ALARM     = 6,   /* the strand's alarm (box/alarm.h) came, at moment */
    GUI_EVENT_GONE      = 7,   /* the keyboard is gone: input is over */
    GUI_EVENT_END_ASKED = 8,   /* the program is asked to finish */
};

typedef struct {
    int      kind;             /* GUI_EVENT_* */
    GuiItem  item;
    float    width, height;    /* the area's or the item's, in points */
    float    scale;            /* pixels to a point */
    int      seen;             /* the area is shown, not behind */
    uint32_t character;        /* a key's, Unicode; 0 for a key that types none */
    uint8_t  scancode, mods;
    uint64_t moment;           /* the alarm's, of the ClockBoard's uptime in ms */
} GuiEvent;

/* An area wished Width x Height points (0 for the display's choice) where
 * Placement says; Always: shown even while the program lives behind. NULL
 * when the display gives none. */
GuiArea *gui_open(int placement, float width, float height, int always);
/* The area gone, and every item in it. */
void     gui_close(GuiArea *area);
float    gui_width(const GuiArea *area);
float    gui_height(const GuiArea *area);
float    gui_scale(const GuiArea *area);      /* pixels to a point */
int      gui_on_text_page(const GuiArea *area);

/* Items made under Parent — a group, or 0 for the area — of a role. */
GuiItem gui_text(GuiArea *area, GuiItem parent, const char *utf8, int role);
GuiItem gui_rectangle(GuiArea *area, GuiItem parent, int role);
GuiItem gui_ellipse(GuiArea *area, GuiItem parent, int role);
GuiItem gui_group(GuiArea *area, GuiItem parent, int role);
void    gui_remove(GuiArea *area, GuiItem item);

void gui_set_text(GuiArea *area, GuiItem item, const char *utf8);
void gui_set_place(GuiArea *area, GuiItem item, float x, float y);
void gui_set_size(GuiArea *area, GuiItem item, float width, float height);
void gui_set_fill(GuiArea *area, GuiItem item, uint32_t rgba);
void gui_set_corners(GuiArea *area, GuiItem item, float radius);
void gui_set_opacity(GuiArea *area, GuiItem item, float opacity);
void gui_set_text_size(GuiArea *area, GuiItem item, float points);
void gui_set_text_weight(GuiArea *area, GuiItem item, int weight);
void gui_show(GuiArea *area, GuiItem item);
void gui_hide(GuiArea *area, GuiItem item);

/* What changed sent as one step: 1 when all of it went, 0 when the rest
 * waits for the next. */
int gui_present(GuiArea *area);
/* The next thing that happens, waited for: what the display tells, a key when
 * Keys is nonzero (the keyboard heard from then on), the strand's alarm. 0, or
 * -1 for no area or no event to fill. */
int gui_wait(GuiArea *area, GuiEvent *event, int keys);
/* A text measured beforehand as the display lays it, at most Width points wide
 * (GUI_BY_CONTENT: as wide as it is), into *out_width x *out_height points. */
void gui_measure(const GuiArea *area, const char *utf8, int role, float width, float *out_width, float *out_height);

#ifdef __cplusplus
}
#endif

#endif /* PAINTER_GUI_C_H */
