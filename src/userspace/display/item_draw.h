#ifndef BOXOS_DISPLAY_ITEM_DRAW_H
#define BOXOS_DISPLAY_ITEM_DRAW_H

// An item drawn: its text laid out and its size measured where its content
// decides it, its picture kept where drawing it again would cost — a text, a
// shape of gradients, strokes or shadows, a canvas's page — or drawn straight
// where the frame needs it, as a shape of plain colours, a picture, cells and
// anything turned or scaled are. Cells are the console's look (cells_draw.h),
// their size their grid's. Everything an item says is in points; it is
// drawn at the screen's scale, through Painter's engine (painter/core), each
// pixel as every other hand of BoxOS covers and lays it.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cells_draw.h"
#include "item_tree.h"
#include "painter/core/canvas.h"
#include "painter/core/mask.h"
#include "painter/core/picture.h"
#include "painter/core/shadow.h"
#include "painter/core/transform.h"
#include "painter/items/look.h"
#include "typecase/fonts.h"
#include "typecase/layout.h"

namespace items {

// What a program's items are drawn from besides themselves.
struct Sources {
    const Theme           *Look  = nullptr;
    float                  Scale = 1.0f;     // pixels to a point
    // The system's fonts and the program's own, asked when a text is laid.
    std::function<const typecase::Fonts &()> Book;
    const CellsLook       *Cells = nullptr;  // the console's look, every cells item's
    // A picture block as a picture; none for a block the program does not have.
    std::function<std::shared_ptr<const painter::core::Picture>(std::uint32_t)> PictureOf;
    // A canvas block's page last shown; none likewise.
    std::function<const painter::core::Canvas *(std::uint32_t)> CanvasOf;
};

// A shadowed item drawn straight: its shadows' coverage made for the place a
// frame lays it at, before the frame paints it (KeepShadows), and laid on
// every tile it falls on moved by whole pixels — not made again on each.
struct KeptShadows {
    painter::core::Transform                         At{};     // the place they were made for
    std::vector<painter::core::Shadow>               Cast;     // as the engine casts them, in the order said
    std::vector<std::optional<painter::core::Mask>>  Masks;    // each one's; none where its shape was refused
    std::size_t                                      Bytes = 0;
};

struct Drawing {
    std::optional<typecase::Layout>   Laid;          // a text's
    std::vector<painter::core::Color> Inks;          // the colours its stretches say: ink 1 on
    ItemPoint                         Measured{};    // its size by its content, of ITEM_POINT
    ItemPoint                         Inset{};       // a text's words from its corner: in from its plate's
    painter::core::RectF              Own{};         // what it covers, in its own points, its shadows too
    std::optional<painter::core::Canvas> Kept;       // its picture at the scale it was drawn at
    painter::core::Point              KeptAt{};      // the picture's corner from the item's, in pixels
    float                             KeptScale = 0;
    // The same picture to lay through a turn or a scale while an animation
    // turns or scales the item: made before a frame wants it (KeepTurning).
    std::shared_ptr<const painter::core::Picture> KeptPicture;
    std::optional<KeptShadows>        Shadows;          // drawn straight, its shadows as last laid
    bool                              Opaque = false;   // its plain colours cover its rectangle whole
    std::size_t                      *Held  = nullptr;  // the program's count of kept bytes
    std::size_t                       Bytes = 0;        // of which, this picture's and its shadows'
};

// The item's size in points: said, or by its content.
painter::core::PointF SizeOf(const Item &Of, const Drawing &Drawn) noexcept;

// The item's drawing made again from what it says: laid out, measured, and
// its picture kept when keeping it is worth it and Keep allows it. True when
// its size by its content changed.
bool Prepare(const Item &Of, const Sources &From, Drawing &Into, bool Keep);

// The item drawn onto Target, its own points laid on the glass by Place, at
// Opacity, within the target's clip: from its kept picture where Place only
// moves it at the scale it was kept at — or turns or scales it while an
// animation does (Turning: of it, or of a group it lies in), the picture
// laid through Place, smooth — straight otherwise. Nothing of the drawing
// changes: tiles of a frame are painted on several strands at once.
void Paint(const Item &Of, const Drawing &Drawn, const Sources &From, painter::core::Canvas &Target,
           const painter::core::Transform &Place, float Opacity, bool Turning = false);

// The picture its kept picture is laid through a turn or a scale by, made
// if it is not yet: before a frame that turns or scales it is painted.
void KeepTurning(Drawing &Drawn);
// Its shadows' coverage made for Place, where a frame paints it straight and
// they are not made for it already — before that frame is painted; let go
// where it is not painted straight, or Keep forbids keeping more.
void KeepShadows(const Item &Of, Drawing &Drawn, const Sources &From, const painter::core::Transform &Place,
                 bool Turning, bool Keep);

// Whether painting it sets glyphs from a cache one strand keeps — a cells
// item's, a text drawn straight: only the display's main strand may.
bool OwnGlyphs(const Item &Of, const Drawing &Drawn, const painter::core::Transform &Place, bool Turning) noexcept;

// A group's clip: its rectangle of its size, its corners rounded; none for a
// group of no size.
painter::core::Path ClipOf(const Item &Group, const Theme &Look);

// The pixels Place lays Rect of points on, whole pixels round it.
painter::core::Rect DeviceRect(const painter::core::RectF &Points, const painter::core::Transform &Place) noexcept;

// Whether Place only moves and scales by Scale: what a kept picture may be laid with.
bool Upright(const painter::core::Transform &Place, float Scale) noexcept;

// Where a canvas item's page lies on the glass pixel for pixel — upright, at
// its own size — the pixel of its corner; none when it is drawn otherwise, or
// has no page to show.
std::optional<painter::core::Point> CanvasCorner(const Item &Of, const Drawing &Drawn, const Sources &From,
                                                 const painter::core::Transform &Place);

}  // namespace items

#endif
