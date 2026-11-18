/*
 * item_draw.cpp — an item drawn. See item_draw.h.
 */

#include "item_draw.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "painter/core/path.h"
#include "painter/core/shadow.h"
#include "painter/core/text.h"
#include "painter/items/text.h"
#include "typecase/text.h"

namespace items {

namespace {

using painter::core::Canvas;
using painter::core::Path;
using painter::core::PointF;
using painter::core::RectF;
using painter::core::Transform;

constexpr float PerPoint = 1.0f / ITEM_POINT;

/* The largest picture kept of one item: past it, the item is drawn straight. */
constexpr std::uint32_t KeptSideMost = 8192;
constexpr std::uint64_t KeptPixelsMost = std::uint64_t{4096} * 4096;

PointF Points(const ItemPoint &Of) noexcept { return PointF{Of.X * PerPoint, Of.Y * PerPoint}; }

/* The pixel a place falls nearest, a half always up: an item drawn into part
 * of the screen moved by whole pixels lands on the very pixels it lands on
 * drawn whole. */
std::int32_t Nearest(float Place) noexcept { return static_cast<std::int32_t>(std::floor(Place + 0.5f)); }

painter::core::Extend ExtendOf(std::uint8_t Of) noexcept
{
    switch (Of) {
    case ITEM_EXTEND_REPEAT: return painter::core::Extend::Repeat;
    case ITEM_EXTEND_REFLECT: return painter::core::Extend::Reflect;
    case ITEM_EXTEND_NONE: return painter::core::Extend::None;
    default: return painter::core::Extend::Pad;
    }
}

std::vector<painter::core::GradientStop> StopsOf(const Fill &Of)
{
    std::vector<painter::core::GradientStop> Made;
    Made.reserve(Of.Stops.size());
    for (const ItemStop &Each : Of.Stops)
        Made.push_back(painter::core::GradientStop{Each.Offset / 65535.0f, ColorOf(Each.Color)});
    return Made;
}

/* One fill of the language as the engine's; none for a picture the program
 * does not have. */
std::optional<painter::core::Fill> EngineFill(const Fill &Of, const Sources &From)
{
    using painter::core::Gradient;
    std::optional<painter::core::Fill> Made;
    switch (Of.Kind) {
    case ITEM_FILL_COLOR: Made.emplace(ColorOf(Of.Color)); break;
    case ITEM_FILL_THEME: Made.emplace(ThemeColor(*From.Look, Of.Theme)); break;
    case ITEM_FILL_LINEAR:
        Made.emplace(Gradient::Linear(Points(Of.From), Points(Of.To), StopsOf(Of), ExtendOf(Of.Extend)));
        break;
    case ITEM_FILL_RADIAL:
        Made.emplace(Gradient::Radial(Points(Of.From), static_cast<float>(Of.Radius) * PerPoint, Points(Of.Focus),
                                      StopsOf(Of), ExtendOf(Of.Extend)));
        break;
    case ITEM_FILL_CONIC:
        Made.emplace(Gradient::Conic(Points(Of.From), static_cast<float>(Of.Start) * 360.0f / ITEM_TURN, StopsOf(Of)));
        break;
    case ITEM_FILL_PICTURE: {
        const std::shared_ptr<const painter::core::Picture> Picture = From.PictureOf ? From.PictureOf(Of.Block) : nullptr;
        if (!Picture) return std::nullopt;
        const PointF At = Points(Of.At), Size = Points(Of.Size);
        const Transform Place = Transform::Scale(Size.X / static_cast<float>(Picture->Width()),
                                                 Size.Y / static_cast<float>(Picture->Height()))
                                    .Then(Transform::Translate(At.X, At.Y));
        Made.emplace(painter::core::Fill::Picture(Picture, Place, ExtendOf(Of.Extend), Of.Sharp));
        break;
    }
    default:
        return std::nullopt;
    }
    Made->WithOpacity(Of.Opacity / 65535.0f);
    return Made;
}

std::vector<painter::core::Fill> EngineFills(const std::vector<Fill> &Of, const Sources &From)
{
    std::vector<painter::core::Fill> Made;
    for (const Fill &Each : Of)
        if (std::optional<painter::core::Fill> One = EngineFill(Each, From)) Made.push_back(std::move(*One));
    return Made;
}

painter::core::Stroke EngineStroke(const Stroke &Of)
{
    painter::core::Stroke Made;
    Made.Width      = static_cast<float>(Of.Width) * PerPoint;
    Made.Cap        = static_cast<painter::core::LineCap>(Of.Cap);
    Made.Join       = static_cast<painter::core::LineJoin>(Of.Join);
    Made.Align      = static_cast<painter::core::StrokeAlign>(Of.Align);
    Made.MiterLimit = static_cast<float>(Of.MiterLimit) * PerPoint;
    Made.DashOffset = static_cast<float>(Of.DashOffset) * PerPoint;
    for (std::int32_t Dash : Of.Dashes) Made.Dashes.push_back(static_cast<float>(Dash) * PerPoint);
    return Made;
}

/* A shadow as the engine casts it: its offset, blur and spread on the glass,
 * at the scale. */
painter::core::Shadow EngineShadow(const Shadow &Of, float Scale)
{
    painter::core::Shadow Made;
    Made.Offset = PointF{Of.Offset.X * PerPoint * Scale, Of.Offset.Y * PerPoint * Scale};
    Made.Blur   = static_cast<float>(Of.Blur) * PerPoint * Scale;
    Made.Spread = static_cast<float>(Of.Spread) * PerPoint * Scale;
    Made.Color  = ColorOf(Of.Color);
    Made.Inner  = Of.Inner;
    return Made;
}

/* The theme items are drawn in: the one their program's are, or the system's
 * before one is given. */
const Theme &LookOf(const Sources &From) noexcept
{
    static const Theme System;
    return From.Look ? *From.Look : System;
}

/* A shape item's outline, a picture's or a canvas's rectangle, a plated
 * text's plate, in its own points. */
Path OutlineOf(const Item &Of, PointF Size, const Theme &Look)
{
    const std::array<std::int32_t, 4> Round = CornersOf(Of.Said, Of.Role, Look);
    const RectF                       Box{0, 0, Size.X, Size.Y};
    const items::Shape *Form = Of.Kind == ITEM_SHAPE && Of.Said.Shape ? &*Of.Said.Shape : nullptr;
    if (Form && Form->Form == ITEM_FORM_ELLIPSE) return Path::Ellipse({Size.X / 2, Size.Y / 2}, Size.X / 2, Size.Y / 2);
    if (Form && Form->Form == ITEM_FORM_PATH) {
        Path        Made;
        std::size_t Point = 0;
        const auto  At    = [&]() { return Points(Form->Points[Point++]); };
        for (std::uint8_t Step : Form->Steps) {
            switch (Step) {
            case ITEM_STEP_MOVE: Made.MoveTo(At()); break;
            case ITEM_STEP_LINE: Made.LineTo(At()); break;
            case ITEM_STEP_QUAD: {
                const PointF Pull = At();
                Made.QuadTo(Pull, At());
                break;
            }
            case ITEM_STEP_CUBIC: {
                const PointF First = At(), Second = At();
                Made.CubicTo(First, Second, At());
                break;
            }
            case ITEM_STEP_CLOSE: Made.Close(); break;
            default: break;
            }
        }
        return Made;
    }
    if (Round[0] == 0 && Round[1] == 0 && Round[2] == 0 && Round[3] == 0) return Path::Rect(Box);
    return Path::RoundedRect(Box, painter::core::Corners{Round[0] * PerPoint, Round[1] * PerPoint, Round[2] * PerPoint,
                                                         Round[3] * PerPoint});
}

/* What a text is filled with: its fills said, else its style's colour, else
 * its role's. */
std::vector<painter::core::Fill> TextFills(const Item &Of, const Sources &From)
{
    if (!Of.Said.Fills) {
        const TextStyle Base = StyleOf(Of.Said, Of.Role, *From.Look);
        if (Base.Said & ITEM_SAYS_COLOR) return {painter::core::Fill(ColorOf(Base.Color))};
    }
    return EngineFills(FillsOf(Of.Said, ITEM_TEXT, Of.Role, LookOf(From)), From);
}

/* Where a picture of PictureSize pixels lies in an item of Size points, as
 * Fit says: the placement of its pixels in the item's points. */
Transform Fitted(std::uint8_t Fit, PointF Size, float Wide, float High, float Scale)
{
    float X = Size.X / Wide, Y = Size.Y / High;
    if (Fit == ITEM_FIT_CONTAIN) X = Y = std::min(X, Y);
    else if (Fit == ITEM_FIT_COVER) X = Y = std::max(X, Y);
    else if (Fit == ITEM_FIT_CENTER) X = Y = 1.0f / Scale;
    return Transform::Scale(X, Y).Then(Transform::Translate((Size.X - Wide * X) / 2, (Size.Y - High * Y) / 2));
}

/* The item drawn straight onto Target. */
void DrawStraight(const Item &Of, const Drawing &Drawn, const Sources &From, Canvas &Target, const Transform &Place,
                  float Opacity)
{
    if (Of.Kind == ITEM_GROUP) return;
    const Theme              &Look    = LookOf(From);
    const PointF              Size    = SizeOf(Of, Drawn);
    const std::vector<Shadow> Shadows = ShadowsOf(Of.Said, Of.Role, Look);
    Stroke                    Line;
    const bool                Stroked = StrokeOf(Of.Said, Of.Role, Look, Line);
    /* A text's outline is wanted only to stroke it or cast its shadow — a
     * plated text's is its plate's, which it is filled, stroked and shadowed
     * as. */
    const bool Plate    = Plated(Of.Kind, Of.Role);
    const bool Outlined = Of.Kind != ITEM_TEXT || Plate || Stroked || !Shadows.empty();
    const Path Outline  = !Outlined ? Path()
                          : Of.Kind == ITEM_TEXT && !Plate ? (Drawn.Laid ? painter::core::TextPath(*Drawn.Laid, {0, 0}) : Path())
                                                           : OutlineOf(Of, Size, Look);
    const painter::core::FillRule Rule = Of.Kind == ITEM_SHAPE && Of.Said.Shape && Of.Said.Shape->Rule == 1
                                             ? painter::core::FillRule::EvenOdd
                                             : painter::core::FillRule::NonZero;
    const auto Cast = [&](bool Inner) {
        for (const Shadow &Each : Shadows) {
            if (Each.Inner != Inner) continue;
            const painter::core::Shadow Engine = EngineShadow(Each, From.Scale);
            if (const std::optional<painter::core::Mask> Kept = Engine.MaskFor(Outline, Place))
                Target.DrawShadow(*Kept, Engine, Opacity);
        }
    };
    Cast(false);
    switch (Of.Kind) {
    case ITEM_TEXT:
        if (Plate) Target.Fill(Outline, EngineFills(PlateOf(Of.Role), From), Place, painter::core::FillRule::NonZero, Opacity);
        if (Drawn.Laid) {
            const PointF                           At{Drawn.Inset.X * PerPoint, Drawn.Inset.Y * PerPoint};
            const std::vector<painter::core::Fill> Fills = TextFills(Of, From);
            Target.FillText(*Drawn.Laid, At, Fills, Place, Opacity, 0u);
            for (std::uint32_t Ink = 1; Ink <= Drawn.Inks.size(); Ink++)
                Target.FillText(*Drawn.Laid, At, painter::core::Fill(Drawn.Inks[Ink - 1]), Place, Opacity, Ink);
        }
        break;
    case ITEM_SHAPE:
        Target.Fill(Outline, EngineFills(FillsOf(Of.Said, Of.Kind, Of.Role, Look), From), Place, Rule, Opacity);
        break;
    case ITEM_PICTURE: {
        const std::shared_ptr<const painter::core::Picture> Picture =
            Of.Said.Picture && From.PictureOf ? From.PictureOf(Of.Said.Picture->Block) : nullptr;
        if (!Picture) break;
        const painter::core::Fill Shown = painter::core::Fill::Picture(
            Picture, Fitted(Of.Said.Picture->Fit, Size, static_cast<float>(Picture->Width()),
                            static_cast<float>(Picture->Height()), From.Scale),
            painter::core::Extend::None, Of.Said.Picture->Sharp);
        Target.Fill(Outline, Shown, Place, painter::core::FillRule::NonZero, Opacity);
        break;
    }
    case ITEM_CANVAS: {
        const Canvas *Page = Of.Said.Canvas && From.CanvasOf ? From.CanvasOf(*Of.Said.Canvas) : nullptr;
        if (!Page) break;
        if (const std::optional<painter::core::Point> Corner = CanvasCorner(Of, Drawn, From, Place)) {
            Target.Draw(*Page, *Corner, Opacity);
            break;
        }
        /* Drawn at another size, or turned: its pixels as a picture. */
        std::vector<painter::core::Pixel> Pixels;
        Pixels.reserve(static_cast<std::size_t>(Page->Width()) * Page->Height());
        for (std::uint32_t Y = 0; Y < Page->Height(); Y++)
            Pixels.insert(Pixels.end(), Page->Row(Y), Page->Row(Y) + Page->Width());
        const auto Picture = painter::core::Picture::FromPixels(Pixels, Page->Width(), Page->Height(), Page->Width());
        if (!Picture) break;
        Target.Fill(Outline,
                    painter::core::Fill::Picture(Picture, Fitted(ITEM_FIT_STRETCH, Size, static_cast<float>(Page->Width()),
                                                                 static_cast<float>(Page->Height()), From.Scale)),
                    Place, painter::core::FillRule::NonZero, Opacity);
        break;
    }
    case ITEM_CELLS: {
        if (!Of.Grid || !From.Cells) break;
        const PointF Corner = Place.Apply({0, 0});
        if (Upright(Place, From.Scale)) {
            DrawCells(*Of.Grid, *From.Cells, Target, painter::core::Point{Nearest(Corner.X), Nearest(Corner.Y)}, Opacity);
            break;
        }
        /* Turned or scaled: its cells as a picture of their own, laid as it
         * lies. */
        const auto Wide = static_cast<std::uint32_t>(Of.Grid->Cols()) * static_cast<std::uint32_t>(From.Cells->Width);
        const auto High = static_cast<std::uint32_t>(Of.Grid->Rows()) * static_cast<std::uint32_t>(From.Cells->Height);
        std::optional<Canvas> Own = Canvas::Make(Wide, High);
        if (!Own) break;
        DrawCells(*Of.Grid, *From.Cells, *Own, painter::core::Point{0, 0}, 1.0f);
        std::vector<painter::core::Pixel> Pixels;
        Pixels.reserve(static_cast<std::size_t>(Wide) * High);
        for (std::uint32_t Y = 0; Y < High; Y++) Pixels.insert(Pixels.end(), Own->Row(Y), Own->Row(Y) + Wide);
        const auto Picture = painter::core::Picture::FromPixels(Pixels, Wide, High, Wide);
        if (!Picture) break;
        Target.Fill(Outline,
                    painter::core::Fill::Picture(Picture, Fitted(ITEM_FIT_STRETCH, Size, static_cast<float>(Wide),
                                                                 static_cast<float>(High), From.Scale)),
                    Place, painter::core::FillRule::NonZero, Opacity);
        break;
    }
    default:
        break;
    }
    if (Stroked)
        if (std::optional<painter::core::Fill> With = EngineFill(Line.With, From))
            Target.Stroke(Outline, EngineStroke(Line), *With, Place, Opacity);
    Cast(true);
}

/* Whether an item is drawn straight every time rather than kept: a group, a
 * picture, a canvas (its page is kept already), cells (only those that
 * changed are drawn again), a shape of plain colours. */
bool Straight(const Item &Of, const Theme &Look)
{
    if (Of.Kind == ITEM_GROUP || Of.Kind == ITEM_PICTURE || Of.Kind == ITEM_CANVAS || Of.Kind == ITEM_CELLS) return true;
    if (Of.Kind != ITEM_SHAPE) return false;
    Stroke Line;
    if (StrokeOf(Of.Said, Of.Role, Look, Line) || !ShadowsOf(Of.Said, Of.Role, Look).empty()) return false;
    for (const Fill &Each : FillsOf(Of.Said, Of.Kind, Of.Role, Look))
        if (Each.Kind != ITEM_FILL_COLOR && Each.Kind != ITEM_FILL_THEME) return false;
    return true;
}

/* What an item covers in its own points, drawn at Scale: its outline's box,
 * grown by its stroke and its shadows — a shadow as far as its blur reaches on
 * the glass. */
RectF CoverOf(const Item &Of, const Drawing &Drawn, PointF Size, float Scale, const Theme &Look)
{
    RectF Box{0, 0, Size.X, Size.Y};
    if (Of.Kind == ITEM_GROUP) return RectF{};
    if (Of.Kind == ITEM_TEXT && Drawn.Laid && !Plated(Of.Kind, Of.Role)) {
        /* Glyphs reach past their line's box — an italic's lean, an accent
         * above a capital — by less than an em. */
        float Em = 16;
        for (const typecase::LaidRun &Run : Drawn.Laid->Runs()) Em = std::max(Em, Run.Look.Em);
        Box = RectF{-Em, -Em, Drawn.Laid->Width() + 2 * Em, Drawn.Laid->Height() + 2 * Em};
    } else if (Of.Kind == ITEM_SHAPE && Of.Said.Shape && Of.Said.Shape->Form == ITEM_FORM_PATH) {
        Box = OutlineOf(Of, Size, Look).Bounds();
    }
    Stroke Line;
    if (StrokeOf(Of.Said, Of.Role, Look, Line) && Line.Align != ITEM_ALIGN_INSIDE) {
        const float Reach = static_cast<float>(Line.Width) * PerPoint * (Line.Align == ITEM_ALIGN_OUTSIDE ? 1.0f : 0.5f) *
                            static_cast<float>(Line.MiterLimit) * PerPoint;
        Box = RectF{Box.X - Reach, Box.Y - Reach, Box.Width + 2 * Reach, Box.Height + 2 * Reach};
    }
    RectF Whole = Box;
    for (const Shadow &Each : ShadowsOf(Of.Said, Of.Role, Look)) {
        if (Each.Inner) continue;
        const float Reach = static_cast<float>(EngineShadow(Each, Scale).Reach()) / Scale +
                            static_cast<float>(std::max(Each.Spread, 0)) * PerPoint;
        const RectF Cast{Box.X + Each.Offset.X * PerPoint - Reach, Box.Y + Each.Offset.Y * PerPoint - Reach,
                         Box.Width + 2 * Reach, Box.Height + 2 * Reach};
        const float Left = std::min(Whole.X, Cast.X), Top = std::min(Whole.Y, Cast.Y);
        const float Right = std::max(Whole.X + Whole.Width, Cast.X + Cast.Width);
        const float Bottom = std::max(Whole.Y + Whole.Height, Cast.Y + Cast.Height);
        Whole = RectF{Left, Top, Right - Left, Bottom - Top};
    }
    return Whole;
}

/* Whether the item's plain colours cover its rectangle whole: a rectangle
 * of square corners, opaque, filled with a colour that lets nothing through. */
bool OpaqueOf(const Item &Of, const Sources &From)
{
    const Theme &Look = LookOf(From);
    if (Of.Kind != ITEM_SHAPE || !Straight(Of, Look) || OpacityOf(Of.Said) != ITEM_OPAQUE) return false;
    if (Of.Said.Shape && Of.Said.Shape->Form != ITEM_FORM_RECT) return false;
    const std::array<std::int32_t, 4> Round = CornersOf(Of.Said, Of.Role, Look);
    if (Round[0] || Round[1] || Round[2] || Round[3]) return false;
    for (const Fill &Each : FillsOf(Of.Said, Of.Kind, Of.Role, Look)) {
        const painter::core::Color Color = Each.Kind == ITEM_FILL_THEME ? ThemeColor(*From.Look, Each.Theme) : ColorOf(Each.Color);
        if (Each.Opacity == ITEM_OPAQUE && Color.Opacity() == 255 && !Color.IsScreen()) return true;
    }
    return false;
}

}  // namespace

painter::core::Path ClipOf(const Item &Group, const Theme &Look)
{
    if (!Group.Said.Size || Group.Said.Size->X == ITEM_BY_CONTENT || Group.Said.Size->Y == ITEM_BY_CONTENT) return {};
    return OutlineOf(Group, Points(*Group.Said.Size), Look);
}

std::optional<painter::core::Point> CanvasCorner(const Item &Of, const Drawing &Drawn, const Sources &From, const Transform &Place)
{
    const Canvas *Page = Of.Kind == ITEM_CANVAS && Of.Said.Canvas && From.CanvasOf ? From.CanvasOf(*Of.Said.Canvas) : nullptr;
    if (!Page || !Upright(Place, From.Scale)) return std::nullopt;
    const PointF Size = SizeOf(Of, Drawn);
    if (std::fabs(Size.X * From.Scale - static_cast<float>(Page->Width())) >= 0.5f ||
        std::fabs(Size.Y * From.Scale - static_cast<float>(Page->Height())) >= 0.5f)
        return std::nullopt;
    const PointF Corner = Place.Apply({0, 0});
    return painter::core::Point{Nearest(Corner.X), Nearest(Corner.Y)};
}

bool Upright(const Transform &Place, float Scale) noexcept
{
    return Place.B == 0 && Place.C == 0 && std::fabs(Place.A - Scale) < 1e-4f && std::fabs(Place.D - Scale) < 1e-4f;
}

painter::core::Rect DeviceRect(const RectF &Of, const Transform &Place) noexcept
{
    if (Of.Empty()) return {};
    const PointF Corners[4] = {Place.Apply({Of.X, Of.Y}), Place.Apply({Of.X + Of.Width, Of.Y}),
                               Place.Apply({Of.X, Of.Y + Of.Height}), Place.Apply({Of.X + Of.Width, Of.Y + Of.Height})};
    float Left = Corners[0].X, Right = Corners[0].X, Top = Corners[0].Y, Bottom = Corners[0].Y;
    for (const PointF &Each : Corners) {
        Left = std::min(Left, Each.X), Right = std::max(Right, Each.X);
        Top = std::min(Top, Each.Y), Bottom = std::max(Bottom, Each.Y);
    }
    const float Reach = 1 << 24;
    if (!(Left > -Reach && Right < Reach && Top > -Reach && Bottom < Reach)) return {};
    const auto X = static_cast<std::int32_t>(std::floor(Left)) - 1, Y = static_cast<std::int32_t>(std::floor(Top)) - 1;
    return painter::core::Rect{X, Y, static_cast<std::int32_t>(std::ceil(Right)) + 1 - X,
                               static_cast<std::int32_t>(std::ceil(Bottom)) + 1 - Y};
}

PointF SizeOf(const Item &Of, const Drawing &Drawn) noexcept
{
    /* Cells are as large as their grid, whatever is said. */
    const ItemPoint Said = Of.Said.Size && Of.Kind != ITEM_CELLS ? *Of.Said.Size : ItemPoint{ITEM_BY_CONTENT, ITEM_BY_CONTENT};
    return PointF{(Said.X == ITEM_BY_CONTENT ? Drawn.Measured.X : Said.X) * PerPoint,
                  (Said.Y == ITEM_BY_CONTENT ? Drawn.Measured.Y : Said.Y) * PerPoint};
}

bool Prepare(const Item &Of, const Sources &From, Drawing &Into, bool Keep)
{
    const ItemPoint Before = Into.Measured;
    Into.Laid.reset();
    Into.Kept.reset();
    Into.KeptPicture.reset();
    Into.Measured = {};
    if (Of.Kind == ITEM_TEXT) {
        painter::items::LaidText Laid = painter::items::LayText(Of.Said, Of.Role, *From.Look, From.Book());
        Into.Laid.emplace(std::move(Laid.Laid));
        Into.Inks     = std::move(Laid.Inks);
        Into.Measured = Laid.Measured;
        Into.Inset    = Laid.Inset;
    } else if (Of.Kind == ITEM_PICTURE && Of.Said.Picture && From.PictureOf) {
        if (const auto Picture = From.PictureOf(Of.Said.Picture->Block))
            Into.Measured = ItemPoint{static_cast<std::int32_t>(std::lround(Picture->Width() * ITEM_POINT / From.Scale)),
                                      static_cast<std::int32_t>(std::lround(Picture->Height() * ITEM_POINT / From.Scale))};
    } else if (Of.Kind == ITEM_CELLS && Of.Grid && From.Cells) {
        const auto Points = [&](std::uint32_t Count, std::int32_t Pixels) {
            return static_cast<std::int32_t>(std::lround(static_cast<double>(Count) * Pixels * ITEM_POINT / From.Scale));
        };
        Into.Measured = ItemPoint{Points(Of.Grid->Cols(), From.Cells->Width), Points(Of.Grid->Rows(), From.Cells->Height)};
    } else if (Of.Kind == ITEM_CANVAS && Of.Said.Canvas && From.CanvasOf) {
        if (const Canvas *Page = From.CanvasOf(*Of.Said.Canvas))
            Into.Measured = ItemPoint{static_cast<std::int32_t>(std::lround(Page->Width() * ITEM_POINT / From.Scale)),
                                      static_cast<std::int32_t>(std::lround(Page->Height() * ITEM_POINT / From.Scale))};
    }
    const PointF Size = SizeOf(Of, Into);
    Into.Own    = CoverOf(Of, Into, Size, From.Scale, LookOf(From));
    Into.Opaque = OpaqueOf(Of, From);

    if (Keep && !Straight(Of, LookOf(From)) && !Into.Own.Empty()) {
        const painter::core::Rect Pixels = DeviceRect(Into.Own, Transform::Scale(From.Scale));
        if (Pixels.Width > 0 && Pixels.Height > 0 && static_cast<std::uint32_t>(Pixels.Width) <= KeptSideMost &&
            static_cast<std::uint32_t>(Pixels.Height) <= KeptSideMost &&
            static_cast<std::uint64_t>(Pixels.Width) * static_cast<std::uint64_t>(Pixels.Height) <= KeptPixelsMost) {
            Into.Kept = Canvas::Make(static_cast<std::uint32_t>(Pixels.Width), static_cast<std::uint32_t>(Pixels.Height));
            if (Into.Kept) {
                Into.KeptAt    = painter::core::Point{Pixels.X, Pixels.Y};
                Into.KeptScale = From.Scale;
                const Transform Place = Transform::Scale(From.Scale).Then(
                    Transform::Translate(-static_cast<float>(Pixels.X), -static_cast<float>(Pixels.Y)));
                DrawStraight(Of, Into, From, *Into.Kept, Place, 1.0f);
            }
        }
    }
    return Before.X != Into.Measured.X || Before.Y != Into.Measured.Y;
}

void Paint(const Item &Of, const Drawing &Drawn, const Sources &From, Canvas &Target, const Transform &Place,
           float Opacity, bool Turning)
{
    if (Drawn.Kept && Upright(Place, Drawn.KeptScale)) {
        const PointF Corner = Place.Apply({0, 0});
        Target.Draw(*Drawn.Kept,
                    painter::core::Point{Nearest(Corner.X) + Drawn.KeptAt.X, Nearest(Corner.Y) + Drawn.KeptAt.Y},
                    Opacity);
        return;
    }
    if (Drawn.Kept && (Turning || Of.Turning)) {
        const Canvas &Kept = *Drawn.Kept;
        if (!Drawn.KeptPicture) {
            std::vector<painter::core::Pixel> Pixels;
            Pixels.reserve(static_cast<std::size_t>(Kept.Width()) * Kept.Height());
            for (std::uint32_t Y = 0; Y < Kept.Height(); Y++) Pixels.insert(Pixels.end(), Kept.Row(Y), Kept.Row(Y) + Kept.Width());
            Drawn.KeptPicture = painter::core::Picture::FromPixels(Pixels, Kept.Width(), Kept.Height(), Kept.Width());
        }
        if (Drawn.KeptPicture) {
            /* The picture's pixels where the item's points lie, then where
             * Place lays them. */
            const Transform Laid = Transform::Translate(static_cast<float>(Drawn.KeptAt.X), static_cast<float>(Drawn.KeptAt.Y))
                                       .Then(Transform::Scale(1.0f / Drawn.KeptScale))
                                       .Then(Place);
            (void)Target.Fill(painter::core::Path::Rect(painter::core::RectF{0, 0, static_cast<float>(Kept.Width()),
                                                                             static_cast<float>(Kept.Height())}),
                              painter::core::Fill::Picture(Drawn.KeptPicture), Laid, painter::core::FillRule::NonZero, Opacity);
            return;
        }
    }
    DrawStraight(Of, Drawn, From, Target, Place, Opacity);
}

}  // namespace items
