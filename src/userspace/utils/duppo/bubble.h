/* bubble.h — the bubble Duppo's words are said in, as the person sets it: an
 * oval as a comic draws one, a rounded box or a box, round words of a size,
 * in its paper's colour with a line round it of a colour and a thickness, a
 * tail pointing down at Duppo from the side it lies on, a soft shadow beneath.
 * It is one outline — its line goes round the tail without a seam — and every
 * bubble of a form has the same steps, so one grows into another, or turns to
 * Duppo's other side, by its points walking. In points. */
#ifndef DUPPO_BUBBLE_H
#define DUPPO_BUBBLE_H

#include <cstdint>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/core/path.h"

namespace duppo {

struct BubbleLook {
    enum class Form : std::uint8_t { Oval, Rounded, Square };
    Form                 Shape     = Form::Oval;
    painter::core::Color Paper     = painter::core::Color::Rgb(0xFF, 0xFF, 0xFF);
    painter::core::Color Line      = painter::core::Color::Rgb(0x1C, 0x1C, 0x1C);
    painter::core::Color Ink       = painter::core::Color::Rgb(0x1C, 0x1C, 0x1C);
    float                Thickness = 3;    // the line round it; 0 draws none
    float                Size      = 16;   // the words'
    bool                 Tail      = true;
    bool                 Shadow    = true;
};

struct BubbleShape {
    painter::core::Path   Outline;   // its corner at (0, 0)
    painter::core::PointF Tip;       // where the tail points — or, with none, the corner nearest Duppo
    painter::core::PointF Corner;    // where the words' box lies
    painter::core::PointF Reach;     // how far it reaches across and down, tail and line included
};

/* The bubble round words Measured large, its tail down on its left — Duppo
 * to the left, below — or on its right. */
BubbleShape BubbleFor(const BubbleLook &Look, painter::core::PointF Measured, bool TailLeft);

}  // namespace duppo

#endif
