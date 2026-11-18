/*
 * c_measure.cpp — a text measured beforehand, for C (painter/gui.h): apart,
 * so a C program that measures nothing carries no reader of fonts.
 */

#include "c_face.h"
#include "painter/gui/measure.h"

extern "C" void gui_measure(const GuiArea *area, const char *utf8, int role, float width, float *out_width, float *out_height)
{
    painter::core::PointF Size{};
    if (area) {
        try {
            Size = painter::gui::Measure(area->Screen, utf8 ? utf8 : "", static_cast<painter::gui::Role>(role), width);
        } catch (...) {
            Size = {};
        }
    }
    if (out_width) *out_width = Size.X;
    if (out_height) *out_height = Size.Y;
}
