#ifndef PAINTER_GUI_C_FACE_H
#define PAINTER_GUI_C_FACE_H

// What a GuiArea is (painter/gui.h): an area, and the items the C face made in
// it, kept by their objects — groups apart, to make items in.

#include <optional>
#include <unordered_map>

#include "painter/gui.h"
#include "painter/gui/area.h"
#include "painter/tui/keyboard.h"

struct GuiArea {
    explicit GuiArea(painter::gui::Area Opened) : Screen(std::move(Opened)) {}

    // Gone after the area, which lets go of them first: going, they say
    // nothing more to it.
    std::unordered_map<GuiItem, painter::gui::Item>  Items;
    std::unordered_map<GuiItem, painter::gui::Group> Groups;
    std::optional<painter::tui::Keyboard>            Keys;
    painter::gui::Area                               Screen;
};

#endif
