#include "picture.h"

#include <algorithm>
#include <cmath>

#include "painter/tui/glyph.h"

namespace whale {

namespace {

using painter::core::Color;
using painter::core::Point;
using painter::core::Rect;
using painter::tui::Cell;

constexpr Color Spray  = Color::Rgb(0xF2, 0xFA, 0xFF);
constexpr Color Water  = Color::Rgb(0x6C, 0xAE, 0xEA);
constexpr Color Bright = Color::Rgb(0xC8, 0xE8, 0xFF);
constexpr Color Dim    = Color::Rgb(0x6E, 0xA6, 0xE0);

/* A pixel on the whale's edge is sampled this many times along each side; the
 * share of the samples that are the whale's is how opaque the pixel is. */
constexpr int Samples = 3;
constexpr std::uint8_t WholeShare = 255;

Rect Joined(Rect A, Rect B)
{
    if (A.Empty()) return B;
    if (B.Empty()) return A;
    std::int32_t Left = std::min(A.X, B.X), Top = std::min(A.Y, B.Y);
    std::int32_t Right = std::max(A.X + A.Width, B.X + B.Width), Bottom = std::max(A.Y + A.Height, B.Y + B.Height);
    return {Left, Top, Right - Left, Bottom - Top};
}

}  // namespace

Picture::Picture(painter::core::Size Glass)
    : Cells(Glass),
      Dots(static_cast<std::size_t>(Glass.Width) * Glass.Height * 2, Outline),
      Parts(static_cast<std::size_t>(Glass.Width) * Glass.Height * 2, Part::None),
      Shares(static_cast<std::size_t>(Glass.Width) * Glass.Height * 2, WholeShare)
{
}

void Picture::Dress(const Hues &Colours, bool Edged, bool Smooth)
{
    Body.Wear(Colours);
    Outlined = Edged;
    Softened = Smooth;
}

painter::core::Rect Picture::Draw(const Swimmer &Whale, painter::tui::Frame &Overlay)
{
    Overlay.Fill(Shown, Cell::TransparentCell());
    Clear(Shown);

    Body.Pose(Whale);
    std::int32_t Left = static_cast<std::int32_t>(std::floor(Body.Least.X)), Right = static_cast<std::int32_t>(std::ceil(Body.Most.X));
    std::int32_t Top = static_cast<std::int32_t>(std::floor(Body.Least.Y / 2.0f)) * 2, Bottom = static_cast<std::int32_t>(std::ceil(Body.Most.Y / 2.0f)) * 2;
    Rect Pixels = Rect{Left, Top, Right - Left, Bottom - Top}.Intersect(Rect{0, 2, Cells.Width, Cells.Height * 2 - 2});

    Pixels = Paint(Pixels);
    if (Outlined) Edge(Pixels);
    if (Softened) Pixels = Soften(Pixels);
    Reached = {Pixels.X, Pixels.Y / 2, Pixels.Width, Pixels.Height / 2};
    Lay(Reached, Overlay);
    Sprinkle(Whale, Overlay);

    Rect Changed = Joined(Shown, Reached);
    Shown        = Reached;
    return Changed.Intersect(Cells);
}

void Picture::Clear(Rect Area)
{
    for (std::int32_t Y = Area.Y * 2; Y < (Area.Y + Area.Height) * 2; Y++)
        for (std::int32_t X = Area.X; X < Area.X + Area.Width; X++) Parts[static_cast<std::size_t>(Y) * Cells.Width + X] = Part::None;
}

/* Works out the pixels in the box, and gives the smaller box, as tall as whole
 * cells, that holds all that are the whale's. */
Rect Picture::Paint(Rect Pixels)
{
    std::int32_t Left = Pixels.X + Pixels.Width, Top = Pixels.Y + Pixels.Height, Right = Pixels.X, Bottom = Pixels.Y;
    constexpr std::int32_t Block = 4;
    for (std::int32_t BlockY = Pixels.Y; BlockY < Pixels.Y + Pixels.Height; BlockY += Block) {
        for (std::int32_t BlockX = Pixels.X; BlockX < Pixels.X + Pixels.Width; BlockX += Block) {
            if (!Body.Reaches({BlockX + Block / 2.0f, BlockY + Block / 2.0f}, Block * 0.75f)) continue;
            for (std::int32_t Y = BlockY; Y < std::min(BlockY + Block, Pixels.Y + Pixels.Height); Y++) {
                for (std::int32_t X = BlockX; X < std::min(BlockX + Block, Pixels.X + Pixels.Width); X++) {
                    Pixel Here = Body.At({X + 0.5f, Y + 0.5f});
                    std::size_t At = static_cast<std::size_t>(Y) * Cells.Width + X;
                    Parts[At] = Here.Kind;
                    Dots[At]  = Here.Ink;
                    Shares[At] = WholeShare;
                    if (Here.Kind == Part::None) continue;
                    Left   = std::min(Left, X);
                    Right  = std::max(Right, X + 1);
                    Top    = std::min(Top, Y);
                    Bottom = std::max(Bottom, Y + 1);
                }
            }
        }
    }
    if (Right <= Left) return {};
    Top = Top / 2 * 2;
    return {Left, Top, Right - Left, (Bottom - Top + 1) / 2 * 2};
}

/* A darker line round the whale and round the fin where it lies on the body. */
void Picture::Edge(Rect Pixels)
{
    const std::int32_t Rows = Cells.Height * 2;
    for (std::int32_t Y = Pixels.Y; Y < Pixels.Y + Pixels.Height; Y++) {
        for (std::int32_t X = Pixels.X; X < Pixels.X + Pixels.Width; X++) {
            std::size_t At = static_cast<std::size_t>(Y) * Cells.Width + X;
            if (Parts[At] == Part::None) continue;
            bool Edged = false;
            for (int Step = 0; Step < 4 && !Edged; Step++) {
                std::int32_t Nx = X + (Step == 0) - (Step == 1), Ny = Y + (Step == 2) - (Step == 3);
                if (Nx < 0 || Ny < 0 || Nx >= Cells.Width || Ny >= Rows) continue;
                Part Next = Parts[static_cast<std::size_t>(Ny) * Cells.Width + Nx];
                Edged     = Next == Part::None || (Parts[At] == Part::Fin && Next != Part::Fin);
            }
            if (Edged) Dots[At] = Color::Mix(Dots[At], Outline, 130);
        }
    }
}

/* Whether the pixel is the whale's at its middle while one beside it, a
 * diagonal too, is not, or the other way round: only there is some of the
 * pixel the whale's and some not. */
bool Picture::OnEdge(std::int32_t X, std::int32_t Y) const
{
    const bool Painted = Parts[static_cast<std::size_t>(Y) * Cells.Width + X] != Part::None;
    for (std::int32_t Across = std::max(X - 1, 0); Across <= std::min(X + 1, Cells.Width - 1); Across++)
        for (std::int32_t Down = std::max(Y - 1, 0); Down <= std::min(Y + 1, Cells.Height * 2 - 1); Down++)
            if ((Parts[static_cast<std::size_t>(Down) * Cells.Width + Across] != Part::None) != Painted) return true;
    return false;
}

/* How much of the pixel is the whale's, by Samples by Samples places in it,
 * the middle one being where the pixel was first judged; a pixel the whale
 * reaches only at its rim takes the colour of the sample nearest its middle. */
void Picture::Soft(std::int32_t X, std::int32_t Y)
{
    const std::size_t At = static_cast<std::size_t>(Y) * Cells.Width + X;
    int   Hits = 0;
    float Closest = 1e9f;
    Pixel Nearest;
    for (int Down = 0; Down < Samples; Down++) {
        for (int Across = 0; Across < Samples; Across++) {
            const float OffAcross = (Across + 0.5f) / Samples - 0.5f, OffDown = (Down + 0.5f) / Samples - 0.5f;
            const Pixel Here = Body.At({X + 0.5f + OffAcross, Y + 0.5f + OffDown});
            if (Here.Kind == Part::None) continue;
            Hits++;
            if (OffAcross * OffAcross + OffDown * OffDown < Closest) {
                Closest = OffAcross * OffAcross + OffDown * OffDown;
                Nearest = Here;
            }
        }
    }
    if (Hits == 0) return;
    if (Parts[At] == Part::None) {
        Parts[At] = Nearest.Kind;
        Dots[At]  = Outlined ? Color::Mix(Nearest.Ink, Outline, 130) : Nearest.Ink;
    }
    Shares[At] = static_cast<std::uint8_t>((Hits * WholeShare + Samples * Samples / 2) / (Samples * Samples));
}

/* The edge of the whale made soft: each pixel on it is as opaque as the part
 * of it the whale covers, and the pixels just outside, that it touches only at
 * a rim, are painted at their small share. Gives the box grown by the pixel
 * round it that this can reach, in whole cells. */
Rect Picture::Soften(Rect Pixels)
{
    if (Pixels.Empty()) return Pixels;
    const Rect Area = Rect{Pixels.X - 1, Pixels.Y - 2, Pixels.Width + 2, Pixels.Height + 4}.Intersect(Rect{0, 2, Cells.Width, Cells.Height * 2 - 2});
    Rim.clear();
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++)
        for (std::int32_t X = Area.X; X < Area.X + Area.Width; X++)
            if (OnEdge(X, Y)) Rim.push_back(static_cast<std::size_t>(Y) * Cells.Width + X);
    for (const std::size_t At : Rim) Soft(static_cast<std::int32_t>(At % Cells.Width), static_cast<std::int32_t>(At / Cells.Width));
    return Area;
}

/* Two pixels to a cell, the upper and the lower; the half of a cell the whale
 * leaves empty is see-through, and a pixel it covers in part is translucent,
 * so whatever lies beneath shows there. */
void Picture::Lay(Rect Area, painter::tui::Frame &Overlay) const
{
    for (std::int32_t Row = Area.Y; Row < Area.Y + Area.Height; Row++) {
        for (std::int32_t Col = Area.X; Col < Area.X + Area.Width; Col++) {
            std::size_t Upper = static_cast<std::size_t>(Row * 2) * Cells.Width + Col, Lower = Upper + Cells.Width;
            if (Parts[Upper] == Part::None && Parts[Lower] == Part::None) continue;
            Color Top = Parts[Upper] == Part::None ? Color::Through() : Dots[Upper].WithOpacity(Shares[Upper]);
            Color Low = Parts[Lower] == Part::None ? Color::Through() : Dots[Lower].WithOpacity(Shares[Lower]);
            Overlay.Put({Col, Row}, Cell{painter::tui::Glyphs::UpperHalf, Top, Low});
        }
    }
}

/* Drops and bubbles are whole cells: a spray drop pale and fading toward the
 * water, a bubble that glints. They go where the whale has not painted. */
void Picture::Sprinkle(const Swimmer &Whale, painter::tui::Frame &Overlay)
{
    for (const Drop &One : Whale.Drops) {
        Point Place{static_cast<std::int32_t>(One.At.X), static_cast<std::int32_t>(One.At.Y / 2.0f)};
        if (Place.Y < 1 || Place.Y >= Cells.Height || Place.X >= Cells.Width) continue;
        if (!Overlay.Read(Place).IsTransparent()) continue;

        float Share = std::clamp(One.Age / One.Life, 0.0f, 1.0f);
        Color Ink   = One.Spray ? Color::Mix(Spray, Water, static_cast<std::uint8_t>(Share * 255.0f))
                                : (std::sin(One.Age * 9.0f + One.Phase) > 0.0f ? Bright : Dim);
        Overlay.Put(Place, Cell{painter::tui::Glyphs::Full, Ink, Ink});
        Reached = Joined(Reached, {Place.X, Place.Y, 1, 1});
    }
}

}  // namespace whale
