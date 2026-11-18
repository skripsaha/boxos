/*
 * changes.cpp — changes to an area's items written in the items' language.
 * See painter/gui/changes.h.
 */

#include "painter/gui/changes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace painter::gui {

namespace {

/* Bytes of a value, written in order, each structure as the language lays it. */
class Writer {
public:
    template <typename T>
    void Put(const T &Value)
    {
        const auto *From = reinterpret_cast<const std::uint8_t *>(&Value);
        Bytes.insert(Bytes.end(), From, From + sizeof(T));
    }
    // Bytes, then zeros to the next fourth.
    void Text(std::string_view Of)
    {
        Bytes.insert(Bytes.end(), Of.begin(), Of.end());
        while (Bytes.size() % 4u) Bytes.push_back(0);
    }
    std::vector<std::uint8_t> Bytes;
};

std::int32_t Points(float Value) noexcept
{
    const float Limit = static_cast<float>(ITEM_REACH);
    if (!std::isfinite(Value)) return 0;
    return static_cast<std::int32_t>(std::lround(std::clamp(Value * ITEM_POINT, -Limit, Limit)));
}

ItemPoint PointOf(core::PointF Of) noexcept { return ItemPoint{Points(Of.X), Points(Of.Y)}; }

ItemColor ColorOf(core::Color Of) noexcept
{
    const auto Wide = [](std::uint8_t Eight) { return static_cast<std::uint16_t>(Eight * 257u); };
    return ItemColor{Wide(Of.Red()), Wide(Of.Green()), Wide(Of.Blue()), Wide(Of.IsScreen() ? 255 : Of.Opacity())};
}

std::uint16_t Unit(float Value) noexcept
{
    return static_cast<std::uint16_t>(std::lround(std::clamp(std::isfinite(Value) ? Value : 0.0f, 0.0f, 1.0f) * 65535.0f));
}

void PutFill(Writer &Into, const Fill &Of)
{
    ItemFill Head{};
    Head.Extend  = Of.Extend;
    Head.Opacity = Unit(Of.Opacity);
    switch (Of.What) {
    case Fill::Kind::Color:
        Head.Kind = ITEM_FILL_COLOR;
        Into.Put(Head);
        Into.Put(ColorOf(Of.Color));
        return;
    case Fill::Kind::Theme:
        Head.Kind = ITEM_FILL_THEME;
        Into.Put(Head);
        Into.Put(ItemThemeColor{static_cast<std::uint16_t>(Of.Theme), 0});
        return;
    case Fill::Kind::Picture:
        Head.Kind = ITEM_FILL_PICTURE;
        Into.Put(Head);
        Into.Put(ItemPictureFill{Of.Block, PointOf(Of.At), PointOf(Of.Size), static_cast<std::uint8_t>(Of.Sharp), {}});
        return;
    default:
        break;
    }
    Head.Stops = static_cast<std::uint16_t>(Of.Stops.size());
    if (Of.What == Fill::Kind::Linear) {
        Head.Kind = ITEM_FILL_LINEAR;
        Into.Put(Head);
        Into.Put(ItemLinear{PointOf(Of.From), PointOf(Of.To)});
    } else if (Of.What == Fill::Kind::Radial) {
        Head.Kind = ITEM_FILL_RADIAL;
        Into.Put(Head);
        Into.Put(ItemRadial{PointOf(Of.From), Points(Of.Radius), PointOf(Of.Focus)});
    } else {
        Head.Kind = ITEM_FILL_CONIC;
        Into.Put(Head);
        Into.Put(ItemConic{PointOf(Of.From), static_cast<std::int32_t>(std::lround(Of.StartDegrees / 360.0f * ITEM_TURN))});
    }
    for (const Stop &Each : Of.Stops) Into.Put(ItemStop{Unit(Each.Offset), 0, ColorOf(Each.Color)});
}

void PutStyle(Writer &Into, const TextStyle &Of)
{
    ItemTextStyle Head{};
    if (Of.Family) Head.Said |= ITEM_SAYS_FAMILY, Head.FamilyBytes = static_cast<std::uint16_t>(std::min<std::size_t>(Of.Family->size(), 1024));
    if (Of.Size) Head.Said |= ITEM_SAYS_SIZE, Head.Size = Points(*Of.Size);
    if (Of.Weight) Head.Said |= ITEM_SAYS_WEIGHT, Head.Weight = *Of.Weight;
    if (Of.Italic) Head.Said |= ITEM_SAYS_ITALIC, Head.Italic = *Of.Italic;
    if (Of.LetterSpacing) Head.Said |= ITEM_SAYS_SPACING, Head.Spacing = Points(*Of.LetterSpacing);
    if (Of.LineHeight)
        Head.Said |= ITEM_SAYS_LINE_HEIGHT,
            Head.LineHeight = static_cast<std::int32_t>(std::lround(std::clamp(*Of.LineHeight, 0.0f, 64.0f) * ITEM_ONE));
    if (Of.Underline) Head.Said |= ITEM_SAYS_UNDERLINE, Head.Underline = *Of.Underline;
    if (Of.Strike) Head.Said |= ITEM_SAYS_STRIKE, Head.Strike = *Of.Strike;
    if (Of.Color) Head.Said |= ITEM_SAYS_COLOR, Head.Color = ColorOf(*Of.Color);
    Into.Put(Head);
    Into.Text(Of.Family ? Of.Family->substr(0, Head.FamilyBytes) : std::string_view());
}

/* A path of Painter's in the language's steps: its ready shapes —
 * rectangles, rounded ones, ellipses — drawn out as lines and curves. */
void PutPath(Writer &Into, const core::Path &Of, bool EvenOdd)
{
    std::vector<std::uint8_t> Steps;
    std::vector<ItemPoint>    Points2;
    core::PointF              Pen{}, Start{};   /* where the pen is, and where its contour began */
    const auto Move  = [&](core::PointF A) { Steps.push_back(ITEM_STEP_MOVE), Points2.push_back(PointOf(A)), Pen = Start = A; };
    const auto Line  = [&](core::PointF A) { Steps.push_back(ITEM_STEP_LINE), Points2.push_back(PointOf(A)), Pen = A; };
    const auto Cubic = [&](core::PointF A, core::PointF B, core::PointF C) {
        Steps.push_back(ITEM_STEP_CUBIC);
        Points2.push_back(PointOf(A)), Points2.push_back(PointOf(B)), Points2.push_back(PointOf(C));
        Pen = C;
    };
    const auto Close = [&]() { Steps.push_back(ITEM_STEP_CLOSE), Pen = Start; };
    /* A quarter of an ellipse as a cubic: its pulls this far along. */
    constexpr float Kappa = 0.5522847498f;
    const auto Quarter = [&](core::PointF From, core::PointF Corner, core::PointF To) {
        Cubic({From.X + (Corner.X - From.X) * Kappa, From.Y + (Corner.Y - From.Y) * Kappa},
              {To.X + (Corner.X - To.X) * Kappa, To.Y + (Corner.Y - To.Y) * Kappa}, To);
    };
    const std::span<const core::Path::Step> Kinds  = Of.Steps();
    const std::span<const core::PointF>     Points = Of.Points();
    std::size_t                             At     = 0;
    for (core::Path::Step Kind : Kinds) {
        switch (Kind) {
        case core::Path::Step::Move: Move(Points[At++]); break;
        case core::Path::Step::Line: Line(Points[At++]); break;
        case core::Path::Step::Quad: {
            /* A quadratic curve as the cubic it is. */
            const core::PointF From = Pen, Pull = Points[At], To = Points[At + 1];
            At += 2;
            Cubic({From.X + (Pull.X - From.X) * 2 / 3, From.Y + (Pull.Y - From.Y) * 2 / 3},
                  {To.X + (Pull.X - To.X) * 2 / 3, To.Y + (Pull.Y - To.Y) * 2 / 3}, To);
            break;
        }
        case core::Path::Step::Cubic: Cubic(Points[At], Points[At + 1], Points[At + 2]); At += 3; break;
        case core::Path::Step::Close: Close(); break;
        case core::Path::Step::Rect: {
            const core::PointF C = Points[At], S = Points[At + 1];
            At += 2;
            Move(C), Line({C.X + S.X, C.Y}), Line({C.X + S.X, C.Y + S.Y}), Line({C.X, C.Y + S.Y}), Close();
            break;
        }
        case core::Path::Step::Rounded: {
            const core::PointF C = Points[At], S = Points[At + 1], R1 = Points[At + 2], R2 = Points[At + 3];
            At += 4;
            /* The corners as the path keeps them: top left and right, then
             * bottom right and left — shrunk alike where two would meet. */
            float TL = R1.X, TR = R1.Y, BR = R2.X, BL = R2.Y;
            float Fit = 1;
            const auto Side = [&](float A, float B, float Length) { if (A + B > Length && A + B > 0) Fit = std::min(Fit, Length / (A + B)); };
            Side(TL, TR, S.X), Side(BL, BR, S.X), Side(TL, BL, S.Y), Side(TR, BR, S.Y);
            TL *= Fit, TR *= Fit, BR *= Fit, BL *= Fit;
            const float X = C.X, Y = C.Y, W = S.X, H = S.Y;
            Move({X + TL, Y});
            Line({X + W - TR, Y}), Quarter({X + W - TR, Y}, {X + W, Y}, {X + W, Y + TR});
            Line({X + W, Y + H - BR}), Quarter({X + W, Y + H - BR}, {X + W, Y + H}, {X + W - BR, Y + H});
            Line({X + BL, Y + H}), Quarter({X + BL, Y + H}, {X, Y + H}, {X, Y + H - BL});
            Line({X, Y + TL}), Quarter({X, Y + TL}, {X, Y}, {X + TL, Y});
            Close();
            break;
        }
        case core::Path::Step::Ellipse: {
            const core::PointF M = Points[At], R = Points[At + 1];
            At += 2;
            Move({M.X + R.X, M.Y});
            Quarter({M.X + R.X, M.Y}, {M.X + R.X, M.Y + R.Y}, {M.X, M.Y + R.Y});
            Quarter({M.X, M.Y + R.Y}, {M.X - R.X, M.Y + R.Y}, {M.X - R.X, M.Y});
            Quarter({M.X - R.X, M.Y}, {M.X - R.X, M.Y - R.Y}, {M.X, M.Y - R.Y});
            Quarter({M.X, M.Y - R.Y}, {M.X + R.X, M.Y - R.Y}, {M.X + R.X, M.Y});
            Close();
            break;
        }
        }
    }
    Into.Put(ItemShape{ITEM_FORM_PATH, static_cast<std::uint8_t>(EvenOdd), 0, static_cast<std::uint32_t>(Steps.size())});
    for (std::uint8_t Step : Steps) Into.Put(Step);
    while (Into.Bytes.size() % 4u) Into.Bytes.push_back(0);
    for (const ItemPoint &Each : Points2) Into.Put(Each);
}

}  // namespace

Changes::Record Changes::Make(std::uint16_t Op, std::uint32_t Item, std::span<const std::uint8_t> Body)
{
    const std::size_t Length = (sizeof(ItemRecord) + Body.size() + 7u) & ~std::size_t{7};
    Record            Made(Length, 0);
    const ItemRecord  Head{Op, static_cast<std::uint16_t>(std::min<std::size_t>(Length / 8u, 0xFFFFu)), Item};
    std::memcpy(Made.data(), &Head, sizeof(Head));
    if (!Body.empty()) std::memcpy(Made.data() + sizeof(Head), Body.data(), Body.size());
    return Made;
}

void Changes::Put(std::uint32_t Item, std::uint16_t Property, Record Value)
{
    Writer Body;
    Body.Put(ItemChange{Property, 0, static_cast<std::uint32_t>(Value.size())});
    Body.Bytes.insert(Body.Bytes.end(), Value.begin(), Value.end());
    if (sizeof(ItemRecord) + Body.Bytes.size() > ITEM_RING_BYTES / 2u) {
        RefusedCount++;
        return;
    }
    Properties[{Item, Property}] = Make(ITEM_OP_CHANGE, Item, Body.Bytes);
}

void Changes::Create(std::uint32_t Item, gui::Kind Kind, std::uint32_t Parent, std::uint32_t Above, gui::Role Role)
{
    Writer Body;
    Body.Put(ItemCreate{static_cast<std::uint8_t>(Kind), 0, static_cast<std::uint16_t>(Role), Parent, Above, 0});
    Ordered.emplace_back(Item, Make(ITEM_OP_CREATE, Item, Body.Bytes));
}

void Changes::Remove(std::uint32_t Item)
{
    /* What it was to say goes with it. */
    Properties.erase(Properties.lower_bound({Item, 0}), Properties.upper_bound({Item, 0xFFFF}));
    Ordered.emplace_back(Item, Make(ITEM_OP_REMOVE, Item, {}));
}

void Changes::Restack(std::uint32_t Item, std::uint32_t Parent, std::uint32_t Above)
{
    Writer Body;
    Body.Put(ItemPlaceAmong{Parent, Above});
    Ordered.emplace_back(Item, Make(ITEM_OP_RESTACK, Item, Body.Bytes));
}

void Changes::Reset(std::uint32_t Item, std::uint16_t Property)
{
    Writer Body;
    Body.Put(ItemChange{Property, 0, 0});
    Properties[{Item, Property}] = Make(ITEM_OP_RESET, Item, Body.Bytes);
}

void Changes::BlockChanged(std::uint32_t Block, core::Rect Where, std::uint32_t Page)
{
    Writer Body;
    Body.Put(ItemBlockChanged{Where.X, Where.Y, Where.Width, Where.Height, Page, 0});
    Ordered.emplace_back(Block, Make(ITEM_OP_BLOCK, Block, Body.Bytes));
}

void Changes::Place(std::uint32_t Item, core::PointF At)
{
    Writer Value;
    Value.Put(PointOf(At));
    Put(Item, ITEM_PROP_PLACE, std::move(Value.Bytes));
}

void Changes::Size(std::uint32_t Item, core::PointF Size)
{
    Writer Value;
    Value.Put(ItemPoint{Size.X < 0 ? ITEM_BY_CONTENT : Points(Size.X), Size.Y < 0 ? ITEM_BY_CONTENT : Points(Size.Y)});
    Put(Item, ITEM_PROP_SIZE, std::move(Value.Bytes));
}

void Changes::Opacity(std::uint32_t Item, float Opacity)
{
    Writer Value;
    Value.Put(ItemOpacity{Unit(Opacity), 0});
    Put(Item, ITEM_PROP_OPACITY, std::move(Value.Bytes));
}

void Changes::Hidden(std::uint32_t Item, bool Hidden)
{
    Writer Value;
    Value.Put(ItemFlag{static_cast<std::uint8_t>(Hidden), {}});
    Put(Item, ITEM_PROP_HIDDEN, std::move(Value.Bytes));
}

void Changes::Turn(std::uint32_t Item, float Degrees)
{
    Writer Value;
    Value.Put(ItemTurn{static_cast<std::int32_t>(std::lround(std::fmod(std::isfinite(Degrees) ? Degrees : 0.0f, 360.0f) / 360.0f * ITEM_TURN))});
    Put(Item, ITEM_PROP_TURN, std::move(Value.Bytes));
}

void Changes::Scale(std::uint32_t Item, core::PointF Scale)
{
    const auto Of = [](float Value) {
        return static_cast<std::int32_t>(std::lround(std::clamp(std::isfinite(Value) ? Value : 1.0f, -64.0f, 64.0f) * ITEM_ONE));
    };
    Writer Value;
    Value.Put(ItemPoint{Of(Scale.X), Of(Scale.Y)});
    Put(Item, ITEM_PROP_SCALE, std::move(Value.Bytes));
}

void Changes::Anchor(std::uint32_t Item, core::PointF At)
{
    Writer Value;
    Value.Put(PointOf(At));
    Put(Item, ITEM_PROP_ANCHOR, std::move(Value.Bytes));
}

void Changes::Clip(std::uint32_t Item, bool Clip)
{
    Writer Value;
    Value.Put(ItemFlag{static_cast<std::uint8_t>(Clip), {}});
    Put(Item, ITEM_PROP_CLIP, std::move(Value.Bytes));
}

void Changes::Corners(std::uint32_t Item, const core::Corners &Round)
{
    const auto Length = [](float Value) { return std::max(0, Points(Value)); };
    Writer Value;
    Value.Put(ItemCorners{Length(Round.TopLeft), Length(Round.TopRight), Length(Round.BottomRight), Length(Round.BottomLeft)});
    Put(Item, ITEM_PROP_CORNERS, std::move(Value.Bytes));
}

void Changes::Rectangle(std::uint32_t Item)
{
    Writer Value;
    Value.Put(ItemShape{ITEM_FORM_RECT, 0, 0, 0});
    Put(Item, ITEM_PROP_SHAPE, std::move(Value.Bytes));
}

void Changes::Ellipse(std::uint32_t Item)
{
    Writer Value;
    Value.Put(ItemShape{ITEM_FORM_ELLIPSE, 0, 0, 0});
    Put(Item, ITEM_PROP_SHAPE, std::move(Value.Bytes));
}

void Changes::Outline(std::uint32_t Item, const core::Path &Shape, bool EvenOdd)
{
    Writer Value;
    PutPath(Value, Shape, EvenOdd);
    Put(Item, ITEM_PROP_SHAPE, std::move(Value.Bytes));
}

void Changes::Fills(std::uint32_t Item, std::span<const Fill> Fills)
{
    Writer Value;
    Value.Put(ItemFills{static_cast<std::uint16_t>(Fills.size()), 0});
    for (const Fill &Each : Fills) PutFill(Value, Each);
    Put(Item, ITEM_PROP_FILLS, std::move(Value.Bytes));
}

void Changes::Stroke(std::uint32_t Item, const gui::Stroke &Line)
{
    ItemStroke Head{};
    Head.Width      = std::max(0, Points(Line.Width));
    Head.Cap        = Line.Cap;
    Head.Join       = Line.Join;
    Head.Align      = Line.Align;
    Head.Dashes     = static_cast<std::uint8_t>(std::min<std::size_t>(Line.Dashes.size(), ITEM_DASHES_MOST));
    Head.MiterLimit = std::max(ITEM_POINT, Points(Line.MiterLimit));
    Head.DashOffset = Points(Line.DashOffset);
    Writer Value;
    Value.Put(Head);
    for (std::size_t I = 0; I < Head.Dashes; I++) Value.Put(std::max(0, Points(Line.Dashes[I])));
    PutFill(Value, Line.With);
    Put(Item, ITEM_PROP_STROKE, std::move(Value.Bytes));
}

void Changes::Shadows(std::uint32_t Item, std::span<const Shadow> Shadows)
{
    Writer Value;
    Value.Put(ItemShadows{static_cast<std::uint16_t>(Shadows.size()), 0});
    for (const Shadow &Each : Shadows)
        Value.Put(ItemShadow{PointOf(Each.Offset), std::max(0, Points(Each.Blur)), Points(Each.Spread), ColorOf(Each.Color),
                             static_cast<std::uint8_t>(Each.Inner), {}});
    Put(Item, ITEM_PROP_SHADOWS, std::move(Value.Bytes));
}

void Changes::Text(std::uint32_t Item, std::string_view Utf8)
{
    Writer Value;
    Value.Put(ItemBytes{static_cast<std::uint32_t>(Utf8.size())});
    Value.Text(Utf8);
    Put(Item, ITEM_PROP_TEXT, std::move(Value.Bytes));
}

void Changes::Style(std::uint32_t Item, const TextStyle &Style)
{
    Writer Value;
    PutStyle(Value, Style);
    Put(Item, ITEM_PROP_TEXT_STYLE, std::move(Value.Bytes));
}

void Changes::Spans(std::uint32_t Item, std::span<const Span> Spans)
{
    Writer Value;
    Value.Put(ItemSpans{static_cast<std::uint16_t>(Spans.size()), 0});
    for (const Span &Each : Spans) {
        Value.Put(ItemSpanRange{Each.From, Each.To});
        PutStyle(Value, Each.Style);
    }
    Put(Item, ITEM_PROP_TEXT_SPANS, std::move(Value.Bytes));
}

void Changes::Box(std::uint32_t Item, const TextBox &Box)
{
    Writer Value;
    Value.Put(ItemTextBox{Box.Align, Box.Overflow, static_cast<std::uint8_t>(Box.Wrap), 0, Box.MaxLines});
    Put(Item, ITEM_PROP_TEXT_BOX, std::move(Value.Bytes));
}

void Changes::Secret(std::uint32_t Item, bool Secret)
{
    Writer Value;
    Value.Put(ItemFlag{static_cast<std::uint8_t>(Secret), {}});
    Put(Item, ITEM_PROP_SECRET, std::move(Value.Bytes));
}

void Changes::Caption(std::uint32_t Item, std::string_view Utf8)
{
    Writer Value;
    Value.Put(ItemBytes{static_cast<std::uint32_t>(Utf8.size())});
    Value.Text(Utf8);
    Put(Item, ITEM_PROP_CAPTION, std::move(Value.Bytes));
}

void Changes::Picture(std::uint32_t Item, std::uint32_t Block, std::uint8_t Fit, bool Sharp)
{
    Writer Value;
    Value.Put(ItemPictureOf{Block, Fit, static_cast<std::uint8_t>(Sharp), 0});
    Put(Item, ITEM_PROP_PICTURE, std::move(Value.Bytes));
}

void Changes::Canvas(std::uint32_t Item, std::uint32_t Block)
{
    Writer Value;
    Value.Put(ItemBlockOf{Block});
    Put(Item, ITEM_PROP_CANVAS, std::move(Value.Bytes));
}

void Changes::Role(std::uint32_t Item, gui::Role Role)
{
    Writer Value;
    Value.Put(ItemRoleOf{static_cast<std::uint16_t>(Role), 0});
    Put(Item, ITEM_PROP_ROLE, std::move(Value.Bytes));
}

std::size_t Changes::Bytes() const noexcept
{
    std::size_t Total = 0;
    for (const auto &Each : Ordered) Total += Each.second.size();
    for (const auto &Each : Properties) Total += Each.second.size();
    return Total;
}

bool Changes::Flush(std::uint8_t *Ring, std::uint32_t Bytes, std::uint64_t &Write, std::uint64_t Read, std::uint64_t Step)
{
    const auto Into = [&](const Record &Made) {
        std::uint8_t *At = item_ring_place(Ring, Bytes, &Write, Read, static_cast<std::uint32_t>(Made.size()));
        if (!At) return false;
        std::memcpy(At, Made.data(), Made.size());
        WrittenBytes += Made.size();
        return true;
    };
    if (!Empty()) StepDue = true;
    /* What makes, moves and removes items, in the order it was said; each
     * property after it, as last said. */
    std::size_t Done = 0;
    while (Done < Ordered.size() && Into(Ordered[Done].second)) Done++;
    Ordered.erase(Ordered.begin(), Ordered.begin() + static_cast<std::ptrdiff_t>(Done));
    if (!Ordered.empty()) return false;
    while (!Properties.empty() && Into(Properties.begin()->second)) Properties.erase(Properties.begin());
    if (!Properties.empty()) return false;
    if (!StepDue) return true;
    Writer Body;
    Body.Put(ItemStep{Step});
    if (!Into(Make(ITEM_OP_STEP, 0, Body.Bytes))) return false;
    StepDue = false;
    return true;
}

}  // namespace painter::gui
