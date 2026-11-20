#ifndef PAINTER_CORE_CANVAS_H
#define PAINTER_CORE_CANVAS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "painter/core/color.h"
#include "painter/core/fill.h"
#include "painter/core/geometry.h"
#include "painter/core/mask.h"
#include "painter/core/path.h"
#include "painter/core/pixel.h"
#include "painter/core/shadow.h"
#include "painter/core/stroke.h"
#include "painter/core/transform.h"

namespace typecase {
class Layout;
}

namespace painter::core {

// Whether a canvas's light is written to the glass with a fixed pattern of
// thresholds between the two numbers it lies between (On), or as the nearest
// number (Off). The pattern stands still: a pixel that does not change is
// written the same, so only what changed is ever drawn again.
enum class Dither : bool { Off, On };

// Pixels in memory, drawn on: shapes filled and stroked with colours,
// gradients and pictures, masks, other canvases laid over it — every edge
// covered as the brush covers it (boxos_shape.h), every colour laid as light
// in sixteen bits (Pixel). The display composes the screen on canvases, and a
// program draws its own picture on one.
//
// Every draw is kept within the clip (PushClip), and what each changed is
// counted (Changed). A draw that cannot be made — a shape past the reach of a
// shape, a transform stretching too far, a gradient with no stops — is
// refused whole and says so; nothing of it is drawn. One canvas is drawn on by
// one strand at a time; two canvases by two at once.
class Canvas {
public:
    // Width x Height pixels, all clear; none when a side is none or past
    // BOX_SHAPE_REACH, or the memory is not there.
    static std::optional<Canvas> Make(std::uint32_t Width, std::uint32_t Height);
    // A canvas over pixels someone else keeps — a picture shared with the
    // display — Stride pixels from row to row.
    static std::optional<Canvas> Over(Pixel *Memory, std::uint32_t Width, std::uint32_t Height, std::size_t Stride);

    Canvas(Canvas &&) noexcept;
    Canvas &operator=(Canvas &&) noexcept;
    Canvas(const Canvas &)            = delete;
    Canvas &operator=(const Canvas &) = delete;
    ~Canvas();

    std::uint32_t Width() const noexcept { return Wide; }
    std::uint32_t Height() const noexcept { return High; }
    Pixel        *Row(std::uint32_t Y) noexcept { return Pixels + static_cast<std::size_t>(Y) * Stride; }
    const Pixel  *Row(std::uint32_t Y) const noexcept { return Pixels + static_cast<std::size_t>(Y) * Stride; }

    // The canvas, or Area of it, made one colour — put, not laid over, and
    // not kept within the clip.
    void Clear(Color To);
    void Clear(Rect Area, Color To);

    // Shape filled with each of Fills, one over another, at Opacity.
    bool Fill(const Path &Shape, std::span<const core::Fill> Fills, const Transform &Place = {},
              FillRule Rule = FillRule::NonZero, float Opacity = 1.0f);
    bool Fill(const Path &Shape, const core::Fill &With, const Transform &Place = {},
              FillRule Rule = FillRule::NonZero, float Opacity = 1.0f);
    // A line along Shape, filled with each of Fills.
    bool Stroke(const Path &Shape, const core::Stroke &Line, std::span<const core::Fill> Fills,
                const Transform &Place = {}, float Opacity = 1.0f);
    bool Stroke(const Path &Shape, const core::Stroke &Line, const core::Fill &With, const Transform &Place = {},
                float Opacity = 1.0f);
    // Text Typecase laid out (typecase/layout.h), its top left at At, filled
    // with each of Fills — all of it, or only what is drawn in Ink. Where Place
    // lays text upright at one size, no larger than 256 pixels to the em, every
    // glyph is drawn once at its size and a quarter pixel's place and kept (one
    // cache for every canvas of the program), and glyphs that touch — and a
    // line with a line under or through it — are gathered into one coverage
    // first, so what they share is laid once; elsewhere
    // the text is filled as its shape (TextPath). A text whose box cuts it off
    // is kept within its box.
    bool FillText(const typecase::Layout &Text, PointF At, std::span<const core::Fill> Fills,
                  const Transform &Place = {}, float Opacity = 1.0f, std::optional<std::uint32_t> Ink = std::nullopt);
    bool FillText(const typecase::Layout &Text, PointF At, const core::Fill &With, const Transform &Place = {},
                  float Opacity = 1.0f, std::optional<std::uint32_t> Ink = std::nullopt);
    // A mask's coverage filled, where the mask lies.
    bool FillMask(const Mask &Cover, std::span<const core::Fill> Fills, float Opacity = 1.0f);
    bool FillMask(const Mask &Cover, const core::Fill &With, float Opacity = 1.0f);
    // A shadow's mask (Shadow::MaskFor), kept, laid in the shadow's colour —
    // By pixels from where it was made: the shadow of the same shape moved by
    // whole pixels.
    void DrawShadow(const Mask &Kept, const Shadow &Cast, float Opacity = 1.0f, Point By = {});
    // Another canvas laid over this one, its corner at At.
    void Draw(const Canvas &Picture, Point At, float Opacity = 1.0f);
    // Every pixel moved Share of the way to Toward's — of the same size —
    // light and opacity alike: one picture fading into another, each counted
    // only as much as it is seen, ignoring the clip.
    void FadeTo(const Canvas &Toward, float Share);
    // Area blurred where it lies, as a backdrop behind frosted glass: a
    // Gaussian whose σ is half of Radius, each channel of light and the
    // opacity alike, the area's edge held, within the clip.
    void Blur(Rect Area, float Radius);

    // Every draw after kept within Area too, until PopClip.
    void PushClip(Rect Area);
    // … within Shape too, its edges as soft as a fill's; false, and the clip
    // as it was, when the shape is refused.
    bool PushClip(const Path &Shape, const Transform &Place = {}, FillRule Rule = FillRule::NonZero);
    void PopClip();
    // The pixels draws are kept within now.
    Rect Clip() const noexcept;

    // The least rectangle holding every pixel drawn since it was last taken.
    Rect Changed() const noexcept { return Touched; }
    Rect TakeChanged() noexcept;

    // Area of the canvas onto the glass, over black — Glass the glass's pixel
    // under the canvas's corner, Stride pixels from row to row, four bytes a
    // pixel in the layout Format (BOX_PIXEL_*, boxos_pixel.h). The glass is
    // written, never read. Corner is where the canvas's corner lies on the
    // glass: the dither's noise is the glass's, so a picture put on in pieces
    // is dithered as it is put on whole.
    void CopyOut(Rect Area, std::uint32_t *Glass, std::size_t Stride, std::uint32_t Format, core::Dither Mode,
                 Point Corner = {}) const;
    // Pixels 0x00RRGGBB, whole, put onto Area — From the pixel under the
    // canvas's corner, Stride pixels from row to row: a picture another hand
    // left, taken on.
    void CopyIn(Rect Area, const std::uint32_t *From, std::size_t Stride);

private:
    struct Work;
    Canvas(Pixel *Memory, std::unique_ptr<Pixel[]> Own, std::uint32_t Width, std::uint32_t Height,
           std::size_t Stride);
    void Touch(Rect Area) noexcept;
    // Cover's coverage filled with Fills laid by Place, By pixels from where
    // it lies.
    bool LayMask(const Mask &Cover, Point By, std::span<const core::Fill> Fills, const Transform &Place, float Opacity);
    // Fills made ready, laid by Place, for the coverages LayCover lays next;
    // false when one cannot be drawn.
    bool PrepareFills(std::span<const core::Fill> Fills, const Transform &Place);
    // Coverage over Lies — Lies.Width values a row, Stride from one row to
    // the next — filled with the fills made ready, at Opacity, within the clip.
    void LayCover(const std::uint8_t *Coverage, std::size_t Stride, Rect Lies, float Opacity);

    Pixel                   *Pixels = nullptr;
    std::unique_ptr<Pixel[]> Owned;
    std::uint32_t            Wide   = 0;
    std::uint32_t            High   = 0;
    std::size_t              Stride = 0;
    Rect                     Touched;
    std::unique_ptr<Work>    Room;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_CANVAS_H
