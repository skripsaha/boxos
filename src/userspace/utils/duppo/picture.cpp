#include "picture.h"

#include <algorithm>
#include <cmath>

#include "painter/tui/fine_canvas.h"
#include "painter/tui/mosaic.h"

#include "sampler.h"

namespace duppo {

namespace {

using painter::core::Rect;
using painter::tui::Mosaic;

Rect Joined(Rect A, Rect B)
{
    if (A.Empty()) return B;
    if (B.Empty()) return A;
    std::int32_t Left = std::min(A.X, B.X), Top = std::min(A.Y, B.Y);
    std::int32_t Right = std::max(A.X + A.Width, B.X + B.Width), Bottom = std::max(A.Y + A.Height, B.Y + B.Height);
    return {Left, Top, Right - Left, Bottom - Top};
}

/* The cells Duppo can touch: all it reaches, the cell round it that a soft
 * edge reaches into taken in. */
Rect Reaching(const Flight &Body, const Orbit &Moons, Rect Glass)
{
    const Reach Of = ReachOf(Body, Moons);
    return painter::tui::FineCanvas::Reaching(Of.Left, Of.Top, Of.Right, Of.Bottom).Intersect(Glass);
}

}  // namespace

Rect Picture::Draw(const Flight &Body, const Face &Eye, const Spin &Turning, const Orbit &Moons, painter::tui::Frame &Overlay)
{
    Overlay.Fill(Shown, painter::tui::Cell::TransparentCell());

    const bool    Whole = !Softened || Overlay.Glass() == painter::tui::GlassKind::Text;
    const Sampler Seen(Body, Eye, Turning, Moons, Colours, Whole, Vec{0.5f, 1.0f});
    const Rect    Reached = Reaching(Body, Moons, {0, 1, Cells.Width, Cells.Height - 1});

    for (std::int32_t Row = Reached.Y; Row < Reached.Y + Reached.Height; Row++) {
        for (std::int32_t Col = Reached.X; Col < Reached.X + Reached.Width; Col++) {
            Mosaic Wanted;
            bool Touched = false;
            for (std::size_t Place = 0; Place < Wanted.Count(); Place++) {
                const Vec Middle{static_cast<float>(Col) + (Place % 2 == 0 ? 0.25f : 0.75f),
                                 static_cast<float>(Row * 2) + (Place < 2 ? 0.5f : 1.5f)};
                bool Here = false;
                Wanted.At[Place] = Seen.At(Middle, Here);
                Touched       = Touched || Here;
            }
            if (Touched) Overlay.Put({Col, Row}, painter::tui::ResolveMosaic(Wanted, Overlay.Glass(), Overlay.Mosaics()));
        }
    }

    const Rect Changed = Joined(Shown, Reached);
    Shown              = Reached;
    return Changed.Intersect(Cells);
}

}  // namespace duppo
