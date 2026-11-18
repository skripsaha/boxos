/*
 * c_face.cpp — painter::gui for C. See painter/gui.h.
 */

#include "c_face.h"

#include <new>
#include <type_traits>
#include <utility>

#include "painter/items/properties.h"

namespace {

namespace gui = painter::gui;

static_assert(static_cast<int>(gui::Event::Kind::Area) == GUI_EVENT_AREA && static_cast<int>(gui::Event::Kind::Size) == GUI_EVENT_SIZE &&
              static_cast<int>(gui::Event::Kind::Refused) == GUI_EVENT_REFUSED && static_cast<int>(gui::Event::Kind::Shown) == GUI_EVENT_SHOWN &&
              static_cast<int>(gui::Event::Kind::PageFree) == GUI_EVENT_PAGE_FREE && static_cast<int>(gui::Event::Kind::Key) == GUI_EVENT_KEY &&
              static_cast<int>(gui::Event::Kind::Alarm) == GUI_EVENT_ALARM && static_cast<int>(gui::Event::Kind::Gone) == GUI_EVENT_GONE &&
              static_cast<int>(gui::Event::Kind::EndAsked) == GUI_EVENT_END_ASKED &&
                  static_cast<int>(gui::Event::Kind::AnimationEnd) == GUI_EVENT_ANIMATION_END &&
                  static_cast<int>(gui::Event::Kind::Focus) == GUI_EVENT_FOCUS &&
                  static_cast<int>(gui::Event::Kind::Press) == GUI_EVENT_PRESS && static_cast<int>(gui::Event::Kind::Text) == GUI_EVENT_TEXT,
              "the C face's events are painter::gui's, one for one");

gui::Role RoleOf(int Role) noexcept { return static_cast<gui::Role>(Role); }

template <typename Object>
GuiItem Keep(GuiArea *Area, Object Made)
{
    const GuiItem Number = Made.Number();
    if (Number == 0) return 0;
    if constexpr (std::is_same_v<Object, gui::Group>) Area->Groups.emplace(Number, std::move(Made));
    else Area->Items.emplace(Number, gui::Item(std::move(Made)));
    return Number;
}

/* An item made under Parent — the area, or a group the face made — and kept. */
template <typename Making>
GuiItem Make(GuiArea *Area, GuiItem Parent, Making &&Make) noexcept
{
    if (!Area) return 0;
    try {
        if (Parent == 0) return Keep(Area, Make(Area->Screen));
        const auto Group = Area->Groups.find(Parent);
        return Group == Area->Groups.end() ? 0 : Keep(Area, Make(Group->second));
    } catch (...) {
        return 0;
    }
}

/* The size and weight Item's style says, read back as the display reads it:
 * a style set anew keeps the other. */
gui::TextStyle StyleSaid(const gui::Area &Of, GuiItem Item)
{
    gui::TextStyle Style;
    const std::vector<std::uint8_t> *Was = Of.Said(Item, ITEM_PROP_TEXT_STYLE);
    if (!Was) return Style;
    painter::items::Properties Read;
    std::uint16_t              Role = 0;
    if (painter::items::Decode(ITEM_PROP_TEXT_STYLE, *Was, Read, Role) != OK || !Read.Style) return Style;
    if (Read.Style->Said & ITEM_SAYS_SIZE) Style.Size = static_cast<float>(Read.Style->Size) / ITEM_POINT;
    if (Read.Style->Said & ITEM_SAYS_WEIGHT) Style.Weight = Read.Style->Weight;
    return Style;
}

/* What the area says of an item the face made: its words, by its number. */
gui::Area *Told(GuiArea *Area, GuiItem Item) noexcept
{
    if (!Area || Item == 0 || (!Area->Items.contains(Item) && !Area->Groups.contains(Item))) return nullptr;
    return &Area->Screen;
}

}  // namespace

extern "C" {

GuiArea *gui_open(int placement, float width, float height, int always)
{
    std::expected<gui::Area, box::error> Opened =
        gui::Area::Open(static_cast<gui::Placement>(placement), painter::core::PointF{width, height}, always != 0);
    if (!Opened) return nullptr;
    return new (std::nothrow) GuiArea(std::move(*Opened));
}

void gui_close(GuiArea *area) { delete area; }

float gui_width(const GuiArea *area) { return area ? area->Screen.Size().X : 0; }
float gui_height(const GuiArea *area) { return area ? area->Screen.Size().Y : 0; }
float gui_scale(const GuiArea *area) { return area ? area->Screen.Scale() : 1; }
int   gui_on_text_page(const GuiArea *area) { return area && area->Screen.On() == gui::Glass::Text; }
int      gui_less_motion(const GuiArea *area) { return area && area->Screen.LessMotion() ? 1 : 0; }
uint32_t gui_motion_speed(const GuiArea *area) { return area ? area->Screen.MotionSpeed() : 100u; }

GuiItem gui_text(GuiArea *area, GuiItem parent, const char *utf8, int role)
{
    return Make(area, parent, [&](auto &In) { return In.Text(utf8 ? utf8 : "", RoleOf(role)); });
}

GuiItem gui_rectangle(GuiArea *area, GuiItem parent, int role)
{
    return Make(area, parent, [&](auto &In) { return In.Rectangle(RoleOf(role)); });
}

GuiItem gui_ellipse(GuiArea *area, GuiItem parent, int role)
{
    return Make(area, parent, [&](auto &In) { return In.Ellipse(RoleOf(role)); });
}

GuiItem gui_group(GuiArea *area, GuiItem parent, int role)
{
    return Make(area, parent, [&](auto &In) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(In)>, gui::Group>) return In.Subgroup(RoleOf(role));
        else return In.Group(RoleOf(role));
    });
}

void gui_remove(GuiArea *area, GuiItem item)
{
    if (!area) return;
    area->Items.erase(item);
    area->Groups.erase(item);
}

void gui_set_text(GuiArea *area, GuiItem item, const char *utf8)
{
    if (gui::Area *To = Told(area, item)) To->Changes::Text(item, utf8 ? utf8 : "");
}

void gui_set_place(GuiArea *area, GuiItem item, float x, float y)
{
    if (gui::Area *To = Told(area, item)) To->Place(item, painter::core::PointF{x, y});
}

void gui_set_size(GuiArea *area, GuiItem item, float width, float height)
{
    if (gui::Area *To = Told(area, item)) To->Changes::Size(item, painter::core::PointF{width, height});
}

void gui_set_fill(GuiArea *area, GuiItem item, uint32_t rgba)
{
    gui::Area *To = Told(area, item);
    if (!To) return;
    const gui::Fill One = gui::Fill::Of(painter::core::Color::Rgba(static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
                                                                    static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)));
    To->Fills(item, std::span(&One, 1));
}

void gui_set_corners(GuiArea *area, GuiItem item, float radius)
{
    if (gui::Area *To = Told(area, item)) To->Corners(item, painter::core::Corners::All(radius));
}

void gui_set_opacity(GuiArea *area, GuiItem item, float opacity)
{
    if (gui::Area *To = Told(area, item)) To->Opacity(item, opacity);
}

void gui_set_text_size(GuiArea *area, GuiItem item, float points)
{
    gui::Area *To = Told(area, item);
    if (!To) return;
    gui::TextStyle Style = StyleSaid(*To, item);
    Style.Size           = points;
    To->Style(item, Style);
}

void gui_set_text_weight(GuiArea *area, GuiItem item, int weight)
{
    gui::Area *To = Told(area, item);
    if (!To) return;
    gui::TextStyle Style = StyleSaid(*To, item);
    Style.Weight         = static_cast<std::uint16_t>(weight);
    To->Style(item, Style);
}

void gui_show(GuiArea *area, GuiItem item)
{
    if (gui::Area *To = Told(area, item)) To->Hidden(item, false);
}

void gui_hide(GuiArea *area, GuiItem item)
{
    if (gui::Area *To = Told(area, item)) To->Hidden(item, true);
}

void gui_set_focusable(GuiArea *area, GuiItem item, int reached)
{
    if (gui::Area *To = Told(area, item)) To->Focusable(item, reached != 0);
}

void gui_set_disabled(GuiArea *area, GuiItem item, int disabled)
{
    if (gui::Area *To = Told(area, item)) To->Disabled(item, disabled != 0);
}

void gui_focus(GuiArea *area, GuiItem item)
{
    if (area) area->Screen.Focus(item);
}

void gui_animate(GuiArea *area, const GuiAnimation *how)
{
    if (!area) return;
    if (!how) {
        area->Screen.Animating(std::nullopt);
        return;
    }
    gui::Animation Made;
    Made.Duration = std::chrono::milliseconds(how->milliseconds);
    Made.Curve    = how->curve >= 0 && how->curve < ITEM_CURVES ? static_cast<gui::Animation::Ease>(how->curve) : gui::Animation::Ease::Linear;
    Made.Bounce   = how->bounce;
    Made.Steps    = how->steps;
    Made.Bend1    = painter::core::PointF{how->x1, how->y1};
    Made.Bend2    = painter::core::PointF{how->x2, how->y2};
    Made.Delay    = std::chrono::milliseconds(how->delay);
    Made.Number   = how->number;
    Made.After    = how->after;
    Made.Repeats  = how->repeats;
    Made.Back     = how->back != 0;
    area->Screen.Animating(Made);
}

int gui_present(GuiArea *area) { return area && area->Screen.Present() ? 1 : 0; }

int gui_wait(GuiArea *area, GuiEvent *event, int keys)
{
    if (!area || !event) return -1;
    if (keys && !area->Keys) {
        if (std::expected<painter::tui::Keyboard, painter::tui::StageError> Opened = painter::tui::Keyboard::Open())
            area->Keys.emplace(std::move(*Opened));
    }
    const gui::Event Heard = area->Screen.Wait(keys && area->Keys ? &*area->Keys : nullptr);
    *event           = GuiEvent{};
    event->kind      = static_cast<int>(Heard.What);
    event->item      = Heard.Item;
    event->width     = Heard.Size.X;
    event->height    = Heard.Size.Y;
    event->scale     = Heard.Scale;
    event->seen      = Heard.Seen ? 1 : 0;
    event->character = static_cast<uint32_t>(Heard.Key.Character);
    event->scancode  = Heard.Key.Scancode;
    event->mods      = Heard.Key.Mods;
    event->moment    = Heard.Moment;
    event->number    = Heard.Number;
    event->finished  = Heard.Finished ? 1 : 0;
    event->came      = Heard.Came ? 1 : 0;
    area->Said       = Heard.Text;
    event->text      = area->Said.c_str();
    return 0;
}

}  // extern "C"
