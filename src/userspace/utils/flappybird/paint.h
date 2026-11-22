/* paint.h — the few ways the game fills and outlines what it shows: a
 * gradient down or across an item, a pipe's green, the dark edge, and the
 * style of its words. */
#ifndef FLAPPYBIRD_PAINT_H
#define FLAPPYBIRD_PAINT_H

#include <initializer_list>

#include "painter/gui/changes.h"

#include "look.h"

namespace flappybird::paint {

namespace gui = painter::gui;

/* Colours from From to To points down the item, or across it. */
inline gui::Fill Down(float From, float To, std::initializer_list<gui::Stop> Stops)
{
    gui::Fill Made;
    Made.What  = gui::Fill::Kind::Linear;
    Made.From  = {0, From};
    Made.To    = {0, To};
    Made.Stops = Stops;
    return Made;
}

inline gui::Fill Across(float From, float To, std::initializer_list<gui::Stop> Stops)
{
    gui::Fill Made = Down(0, 0, Stops);
    Made.From      = {From, 0};
    Made.To        = {To, 0};
    return Made;
}

/* A round light, brightest at Focus, out to Radius from Middle. */
inline gui::Fill Round(painter::core::PointF Middle, float Radius, painter::core::PointF Focus, std::initializer_list<gui::Stop> Stops)
{
    gui::Fill Made = Down(0, 0, Stops);
    Made.What      = gui::Fill::Kind::Radial;
    Made.From      = Middle;
    Made.Radius    = Radius;
    Made.Focus     = Focus;
    return Made;
}

/* A pipe Wide points across: lit along its left, shaded to its right. */
inline gui::Fill Pipe(float Wide)
{
    return Across(0, Wide, {{0, ink::PipeEdge}, {0.1f, ink::PipeLit}, {0.24f, ink::PipeLit}, {0.55f, ink::PipeBody},
                            {0.86f, ink::PipeShade}, {1, ink::PipeShade}});
}

inline gui::Stroke Edge(float Width, Color With = ink::Outline)
{
    gui::Stroke Made;
    Made.Width = Width;
    Made.Join  = ITEM_JOIN_ROUND;
    Made.Align = ITEM_ALIGN_INSIDE;
    Made.With  = gui::Fill::Of(With);
    return Made;
}

inline gui::TextStyle Words(float Size, Color With, std::uint16_t Weight = 700)
{
    gui::TextStyle Made;
    Made.Size   = Size;
    Made.Color  = With;
    Made.Weight = Weight;
    return Made;
}

/* The dark under the game's words, as if cut out of card. */
inline gui::Shadow Under(float Unit)
{
    return gui::Shadow{{0, 1.3f * Unit}, 0, 0.55f * Unit, ink::Outline, false};
}

}  // namespace flappybird::paint

#endif
