/*
 * text_draw.cpp — text drawn on a canvas, and text as a shape. See
 * painter/core/text.h and Canvas::FillText.
 */

#include "painter/core/text.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "painter/core/canvas.h"
#include "typecase/glyph_cache.h"

namespace painter::core {

namespace {

/* Glyphs drawn for every canvas of the program, kept: four megabytes is a
 * few thousand glyphs of running text. Canvases drawn by two strands at once
 * take it in turn. */
constexpr std::size_t KeptBytes = std::size_t{4} << 20;
std::mutex            KeptGuard;

typecase::GlyphCache &Kept()
{
    static typecase::GlyphCache Cache(KeptBytes);
    return Cache;
}

/* Larger than this, a glyph is filled as its shape each time, not kept. */
constexpr float KeptEmMost = 256.0f;

/* Whether Place lays text upright at one size — alike across and up, and
 * moved — and that size. */
bool Upright(const Transform &Place, float &Scale) noexcept
{
    if (Place.B != 0 || Place.C != 0 || !(Place.A > 0) || Place.A != Place.D) return false;
    Scale = Place.A;
    return true;
}

bool Wanted(const typecase::LaidRun &Run, std::optional<std::uint32_t> Ink) noexcept
{
    return !Ink || Run.Ink == *Ink;
}

/* A glyph's picture where it lands on the canvas. */
struct Landed {
    const typecase::GlyphImage *Image;
    std::int32_t                X, Y;
};

/* Glyph G of Text, its layout's corner at At, Scale and Move from the
 * canvas: its picture at the size and quarter pixel it lands at — the pen on
 * a row's edge, across to a quarter of a pixel — kept, and where it lies. */
Landed Land(const typecase::Layout &Text, const typecase::LaidGlyph &G, PointF At, float Scale, PointF Move)
{
    const typecase::LaidRun &Run  = Text.Runs()[G.Run];
    const float              X    = (At.X + G.X) * Scale + Move.X;
    const auto               Y    = static_cast<std::int32_t>(std::lround((At.Y + G.Y) * Scale + Move.Y));
    float                    Left = std::floor(X);
    auto                     Quarter = static_cast<std::int32_t>(std::lround((X - Left) * 4));
    if (Quarter == 4) Left += 1, Quarter = 0;
    if (!Run.Face) {
        const auto Whole = static_cast<std::int32_t>(std::lround(X));
        return Landed{&Kept().GetMissing(G.Glyph, typecase::MissingCell(G.Glyph, Run.Look.Em * Scale)), Whole, Y};
    }
    typecase::GlyphLook Look = Run.Look;
    Look.Em                  = Run.Look.Em * Scale;
    Look.Phase               = static_cast<std::uint8_t>(Quarter);
    const auto &Image        = Kept().Get(*Run.Face, static_cast<typecase::GlyphId>(G.Glyph), Look);
    return Landed{&Image, static_cast<std::int32_t>(Left), Y};
}

Rect PlaceOf(const Landed &Glyph) noexcept
{
    return Rect{Glyph.X + Glyph.Image->Left, Glyph.Y + Glyph.Image->Top, Glyph.Image->Width, Glyph.Image->Height};
}

/* A line under or through text where it lands: its rows whole, its ends
 * covering the share of the pixel they reach into. */
struct Bar {
    float        Left, Right;
    std::int32_t Top, Rows;
};

Bar BarOf(const typecase::Area &Of, PointF At, float Scale, PointF Move) noexcept
{
    const float Left = (At.X + Of.X) * Scale + Move.X;
    const auto  Top  = static_cast<std::int32_t>(std::lround((At.Y + Of.Y) * Scale + Move.Y));
    const auto  Rows = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(Of.Height * Scale)));
    return Bar{Left, Left + Of.Width * Scale, Top, Rows};
}

Rect PlaceOf(const Bar &Of) noexcept
{
    const auto Left = static_cast<std::int32_t>(std::floor(Of.Left));
    return Rect{Left, Of.Top, static_cast<std::int32_t>(std::ceil(Of.Right)) - Left, Of.Rows};
}

/* Count coverages laid into a mask's row as one more cover over what is
 * there: the share either covers, 1 − (1 − a)(1 − b). */
void Gather(std::uint8_t *Into, const std::uint8_t *Cover, std::int32_t Count) noexcept
{
    for (std::int32_t I = 0; I < Count; I++) {
        const std::uint32_t Here = Into[I], More = Cover[I];
        if (More) Into[I] = static_cast<std::uint8_t>(Here + More - (Here * More + 127) / 255);
    }
}

/* Where Text lands on the canvas, its corner at At, Scale and Move from it. */
struct Landing {
    const typecase::Layout       &Text;
    PointF                        At;
    float                         Scale;
    PointF                        Move;
    std::optional<std::uint32_t>  Ink;
};

/* A line's glyphs, and the lines under and through it, gathered into one
 * coverage within Seen; none when none of it lies there. */
std::optional<Mask> LineCover(const Landing &On, const typecase::LaidLine &Line, const std::vector<Bar> &Bars, Rect Seen)
{
    const auto Each = [&](auto &&Work) {
        for (std::uint32_t G = Line.FirstGlyph; G < Line.FirstGlyph + Line.GlyphCount; G++) {
            const typecase::LaidGlyph &Glyph = On.Text.Glyphs()[G];
            if (Wanted(On.Text.Runs()[Glyph.Run], On.Ink)) Work(Land(On.Text, Glyph, On.At, On.Scale, On.Move));
        }
    };
    Rect Area{};
    Each([&](const Landed &Here) { Area = Area.Join(PlaceOf(Here)); });
    for (const Bar &Under : Bars) Area = Area.Join(PlaceOf(Under));
    Area = Area.Intersect(Seen);
    if (Area.Empty()) return std::nullopt;

    Mask Cover(Area);
    /* Asked again: a picture holds only until the next is asked. */
    Each([&](const Landed &Here) {
        const Rect                  Lies  = PlaceOf(Here).Intersect(Area);
        const typecase::GlyphImage &Image = *Here.Image;
        for (std::int32_t Y = Lies.Y; Y < Lies.Y + Lies.Height; Y++) {
            const std::uint8_t *Row = &Image.Coverage[static_cast<std::size_t>(Y - Here.Y - Image.Top) *
                                                          static_cast<std::size_t>(Image.Width) +
                                                      static_cast<std::size_t>(Lies.X - Here.X - Image.Left)];
            Gather(Cover.Row(Y) + (Lies.X - Area.X), Row, Lies.Width);
        }
    });
    std::vector<std::uint8_t> Shares;
    for (const Bar &Under : Bars) {
        const Rect Lies = PlaceOf(Under).Intersect(Area);
        if (Lies.Empty()) continue;
        Shares.resize(static_cast<std::size_t>(Lies.Width));
        for (std::int32_t X = Lies.X; X < Lies.X + Lies.Width; X++) {
            const float Covered = std::min(Under.Right, static_cast<float>(X + 1)) - std::max(Under.Left, static_cast<float>(X));
            Shares[static_cast<std::size_t>(X - Lies.X)] =
                static_cast<std::uint8_t>(std::lround(std::clamp(Covered, 0.0f, 1.0f) * 255));
        }
        for (std::int32_t Y = Lies.Y; Y < Lies.Y + Lies.Height; Y++)
            Gather(Cover.Row(Y) + (Lies.X - Area.X), Shares.data(), Lies.Width);
    }
    return Cover;
}

}  // namespace

bool Canvas::FillText(const typecase::Layout &Text, PointF At, std::span<const core::Fill> Fills, const Transform &Place,
                      float Opacity, std::optional<std::uint32_t> Ink)
{
    float Scale   = 1;
    bool  Keeping = Upright(Place, Scale);
    for (const typecase::LaidRun &Run : Text.Runs()) Keeping = Keeping && Run.Look.Em * Scale <= KeptEmMost;

    const typecase::TextBox &Box  = Text.Box();
    const bool               Held = Box.Overflow != typecase::TextOverflow::Grow && Box.Width > 0;
    if (Held && !PushClip(Path::Rect(RectF{At.X, At.Y, Text.Width(), Text.Height()}), Place)) return false;
    bool Drawn = true;
    if (!Keeping) {
        Drawn = Fill(TextPath(Text, At, Ink), Fills, Place, FillRule::NonZero, Opacity);
    } else {
        const Landing                          On{Text, At, Scale, PointF{Place.E, Place.F}, Ink};
        const std::vector<typecase::LaidLine> &Lines = Text.Lines();
        std::vector<std::vector<Bar>>          Bars(Lines.size());
        for (const typecase::LaidDecoration &Each : Text.Decorations()) {
            if (Ink && Each.Ink != *Ink) continue;
            std::size_t Line = 0;
            while (Line + 1 < Lines.size() && Each.Of.Y >= Lines[Line].Top + Lines[Line].Height) Line++;
            Bars[Line].push_back(BarOf(Each.Of, At, Scale, On.Move));
        }
        const Rect                  Seen = Clip();
        std::lock_guard<std::mutex> Lock(KeptGuard);
        for (std::size_t L = 0; L < Lines.size() && Drawn; L++)
            if (const std::optional<Mask> Cover = LineCover(On, Lines[L], Bars[L], Seen))
                Drawn = LayMask(*Cover, Fills, Place, Opacity);
    }
    if (Held) PopClip();
    return Drawn;
}

bool Canvas::FillText(const typecase::Layout &Text, PointF At, const core::Fill &With, const Transform &Place, float Opacity,
                      std::optional<std::uint32_t> Ink)
{
    return FillText(Text, At, std::span<const core::Fill>(&With, 1), Place, Opacity, Ink);
}

Path TextPath(const typecase::Layout &Text, PointF At, std::optional<std::uint32_t> Ink)
{
    Path Shape;
    for (const typecase::LaidGlyph &Glyph : Text.Glyphs()) {
        const typecase::LaidRun &Run = Text.Runs()[Glyph.Run];
        if (!Wanted(Run, Ink)) continue;
        const PointF Pen{At.X + Glyph.X, At.Y + Glyph.Y};
        if (!Run.Face) {
            /* The box's frame, a tenth of its width thick and a pixel in. */
            const typecase::Size Cell  = typecase::MissingCell(Glyph.Glyph, Run.Look.Em);
            const float          Left  = Pen.X + 1, Top = Pen.Y - static_cast<float>(Cell.Baseline) + 1;
            const float          Wide  = static_cast<float>(Cell.Width) - 2, High = static_cast<float>(Cell.Height) - 2;
            const float          Thick = std::max(1.0f, std::round(static_cast<float>(Cell.Width) / 10));
            if (Wide <= 2 * Thick || High <= 2 * Thick) {
                Shape.AddRect(RectF{Pen.X, Pen.Y - static_cast<float>(Cell.Baseline), static_cast<float>(Cell.Width),
                                    static_cast<float>(Cell.Height)});
                continue;
            }
            Shape.AddRect(RectF{Left, Top, Wide, Thick}).AddRect(RectF{Left, Top + High - Thick, Wide, Thick});
            Shape.AddRect(RectF{Left, Top + Thick, Thick, High - 2 * Thick});
            Shape.AddRect(RectF{Left + Wide - Thick, Top + Thick, Thick, High - 2 * Thick});
            continue;
        }
        const typecase::Outline Drawn = typecase::GlyphOutline(*Run.Face, static_cast<typecase::GlyphId>(Glyph.Glyph), Run.Look);
        std::size_t             Point = 0;
        bool                    Open  = false;
        const auto              To    = [&](std::size_t I) { return PointF{Pen.X + Drawn.Points[I].X, Pen.Y + Drawn.Points[I].Y}; };
        for (typecase::Step Each : Drawn.Steps) {
            switch (Each) {
            case typecase::Step::Move:
                if (Open) Shape.Close();
                Shape.MoveTo(To(Point));
                Open = true;
                Point += 1;
                break;
            case typecase::Step::Line:
                Shape.LineTo(To(Point));
                Point += 1;
                break;
            case typecase::Step::Quad:
                Shape.QuadTo(To(Point), To(Point + 1));
                Point += 2;
                break;
            }
        }
        if (Open) Shape.Close();
    }
    for (const typecase::LaidDecoration &Each : Text.Decorations())
        if (!Ink || Each.Ink == *Ink)
            Shape.AddRect(RectF{At.X + Each.Of.X, At.Y + Each.Of.Y, Each.Of.Width, Each.Of.Height});
    return Shape;
}

}  // namespace painter::core
