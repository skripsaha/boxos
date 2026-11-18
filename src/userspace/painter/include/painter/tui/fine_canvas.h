#ifndef PAINTER_TUI_FINE_CANVAS_H
#define PAINTER_TUI_FINE_CANVAS_H

/* Shapes drawn finer than a cell, in the cells' own glyphs: each cell is cut
 * into sub-pixels two across and, down, as many as the glass draws exactly
 * (box/items.h ITEM_MOSAIC_*): four where it draws the octants, three the
 * sextants, else two, so at the least twice the precision of PutPixel across
 * and the same down. A shape covers each sub-pixel by some share, its ink is
 * blended there over what the cell shows now, and the cell is settled on the
 * glyph and the two colours that show its sub-pixels best (ResolveMosaic), so
 * an edge is a quadrant, an octant, a half, a block or a shade, as the glass
 * has them, and not a staircase of whole cells. Every cell a shape reaches is
 * settled at once, as opaque, and one the shape misses is not touched: a
 * canvas keeps no state, and shapes drawn one over another are laid in
 * order, each over the cells as they are. Over a see-through sub-pixel — one
 * nothing has painted on a transparent frame — a shape's ink is laid at its
 * share as an opacity (Color::Over), so an edge toward an unknown background
 * is soft all the same: the display blends it with whatever lies beneath,
 * and what the shape leaves is left see-through (Color::Through). Text is
 * part of the cells too, and where a shape reaches over it, it is gone: the
 * shapes go first and the words after.
 *
 * Shapes are placed in the units of PutPixel: a cell is 1 across and 2 down,
 * so a circle is round on the glass, and a sub-pixel is a half across and 2
 * over the rows of its grid down, 1 on two rows. It is still text; a program
 * that wants more asks the glass.
 *
 *   FineCanvas Fine(Out);
 *   Fine.Disc(20.0f, 14.0f, 6.0f, Ink); */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/mosaic.h"

namespace painter::tui {

class FineCanvas {
public:
    explicit FineCanvas(Frame &Target) noexcept : Target(&Target) {}

    /* A cell set to the nearest the glass has to the sub-pixels. False when
     * the cell is outside the frame. */
    bool PutMosaic(core::Point Cell, const Mosaic &Wanted);

    /* A shape given by how far a point is outside it, in the units above:
     * negative within, 0 on the edge. Each sub-pixel of the finest grid the
     * frame's glass draws is covered by how far inside the edge its middle
     * lies, the width of a sub-pixel along the edge's own direction counted,
     * and the ink is laid at that share of Opacity; on a text page, where a
     * mix of two colours is none of its sixteen, a sub-pixel is covered whole
     * where that share is half or more and not at all otherwise. Outside need
     * be right only within a cell of the edge, and Cells, those the shape can
     * reach, bounds the work. */
    template <class Distance>
    void Cover(core::Rect Cells, core::Color Ink, Distance Outside, float Opacity = 1.0f);

    /* The cells a box of the units above touches, with the cell around it
     * that a soft edge reaches into. */
    static core::Rect Reaching(float Left, float Top, float Right, float Bottom) noexcept;

    void Fill(float Left, float Top, float Across, float Down, core::Color Ink, float Opacity = 1.0f);
    void Disc(float Across, float Down, float Radius, core::Color Ink, float Opacity = 1.0f);
    void Ellipse(float Across, float Down, float ReachAcross, float ReachDown, core::Color Ink, float Opacity = 1.0f);
    void Ring(float Across, float Down, float Outer, float Inner, core::Color Ink, float Opacity = 1.0f);
    void Rounded(float Left, float Top, float Across, float Down, float Radius, core::Color Ink, float Opacity = 1.0f);
    void Line(core::PointF From, core::PointF To, float Thick, core::Color Ink, float Opacity = 1.0f);
    void Polygon(std::span<const core::PointF> Corners, core::Color Ink, float Opacity = 1.0f);

private:
    Frame *Target;
};

template <class Distance>
void FineCanvas::Cover(core::Rect Cells, core::Color Ink, Distance Outside, float Opacity)
{
    constexpr float SubAcross = 0.5f, Step = 0.05f;
    const std::uint8_t Grid = (Target->Mosaics() & ITEM_MOSAIC_OCTANTS) != 0 ? 4 : (Target->Mosaics() & ITEM_MOSAIC_SEXTANTS) != 0 ? 3 : 2;
    const float SubDown = 2.0f / static_cast<float>(Grid);
    const core::Rect Within = Cells.Intersect(Target->Size());
    const bool Pure = Target->Glass() == GlassKind::Text;
    for (std::int32_t Row = Within.Y; Row < Within.Y + Within.Height; Row++) {
        for (std::int32_t Col = Within.X; Col < Within.X + Within.Width; Col++) {
            Mosaic Shown = MosaicOf(Target->Read({Col, Row}), Grid);
            bool Touched = false;
            for (std::size_t Place = 0; Place < Shown.Count(); Place++) {
                const float Across = static_cast<float>(Col) + (Place % 2 == 0 ? 0.25f : 0.75f);
                const float Down   = static_cast<float>(Row * 2) + (static_cast<float>(Place / 2) + 0.5f) * SubDown;
                const float Far    = Outside(Across, Down);
                if (Far > SubDown) continue;
                float Share = 1.0f;
                if (Far > -SubDown) {
                    const float Along  = (Outside(Across + Step, Down) - Far) / Step;
                    const float Over   = (Outside(Across, Down + Step) - Far) / Step;
                    const float Length = std::sqrt(Along * Along + Over * Over);
                    const float Width  = Length > 0.0f ? (SubAcross * std::fabs(Along) + SubDown * std::fabs(Over)) / Length : SubDown;
                    Share = std::clamp(0.5f - Far / Width, 0.0f, 1.0f);
                }
                float Laid = std::clamp(Share * Opacity, 0.0f, 1.0f);
                if (Pure) Laid = Laid >= 0.5f ? 1.0f : 0.0f;
                const auto Part = static_cast<std::uint8_t>(Laid * 255.0f + 0.5f);
                if (Part == 0) continue;
                Shown.At[Place] = core::Color::Over(Shown.At[Place], Ink, Part);
                Touched = true;
            }
            if (Touched) Target->Put({Col, Row}, ResolveMosaic(Shown, Target->Glass(), Target->Mosaics()));
        }
    }
}

}  // namespace painter::tui

#endif  // PAINTER_TUI_FINE_CANVAS_H
