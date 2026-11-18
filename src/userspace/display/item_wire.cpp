/*
 * item_wire.cpp — the items' language read: a property's value held to its
 * bounds. See item_wire.h.
 */

#include "item_wire.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace items {

namespace {

// A value's bytes read in order: nothing read past their end, and a value is
// whole only when every byte of it was read but the padding to four.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> Of) noexcept : Bytes(Of) {}

    template <typename T>
    bool Take(T &Into) noexcept
    {
        if (Broken || Bytes.size() - At < sizeof(T)) return Broken = true, false;
        std::memcpy(&Into, Bytes.data() + At, sizeof(T));
        At += sizeof(T);
        return true;
    }

    // Count bytes as text, then on to the next fourth byte.
    bool TakeText(std::size_t Count, std::string &Into)
    {
        if (Broken || Bytes.size() - At < Count) return Broken = true, false;
        Into.assign(reinterpret_cast<const char *>(Bytes.data() + At), Count);
        At += Count;
        At = std::min(Bytes.size(), (At + 3) & ~std::size_t{3});
        return true;
    }

    bool Whole() const noexcept { return !Broken && Bytes.size() - At < 4; }

private:
    std::span<const std::uint8_t> Bytes;
    std::size_t                   At     = 0;
    bool                          Broken = false;
};

bool Within(std::int32_t Value) noexcept { return Value >= -ITEM_REACH && Value <= ITEM_REACH; }
bool Within(const ItemPoint &Point) noexcept { return Within(Point.X) && Within(Point.Y); }
bool Length(std::int32_t Value) noexcept { return Value >= 0 && Value <= ITEM_REACH; }

// Gradient stops: at least one, their offsets never going back.
bool TakeStops(Reader &From, std::uint16_t Count, std::vector<ItemStop> &Into)
{
    if (Count == 0 || Count > ITEM_STOPS_MOST) return false;
    Into.resize(Count);
    for (std::uint16_t I = 0; I < Count; I++) {
        if (!From.Take(Into[I])) return false;
        if (I > 0 && Into[I].Offset < Into[I - 1].Offset) return false;
    }
    return true;
}

bool TakeFill(Reader &From, Fill &Into)
{
    ItemFill Head{};
    if (!From.Take(Head) || Head.Extend > ITEM_EXTEND_NONE) return false;
    Into.Kind    = Head.Kind;
    Into.Extend  = Head.Extend;
    Into.Opacity = Head.Opacity;
    switch (Head.Kind) {
    case ITEM_FILL_COLOR:
        return From.Take(Into.Color);
    case ITEM_FILL_THEME: {
        ItemThemeColor Theme{};
        if (!From.Take(Theme) || Theme.Which == 0 || Theme.Which >= ITEM_THEME_COLORS) return false;
        Into.Theme = Theme.Which;
        return true;
    }
    case ITEM_FILL_LINEAR: {
        ItemLinear Line{};
        if (!From.Take(Line) || !Within(Line.From) || !Within(Line.To)) return false;
        Into.From = Line.From, Into.To = Line.To;
        return TakeStops(From, Head.Stops, Into.Stops);
    }
    case ITEM_FILL_RADIAL: {
        ItemRadial Round{};
        if (!From.Take(Round) || !Within(Round.Middle) || !Within(Round.Focus) || !Length(Round.Radius)) return false;
        Into.From = Round.Middle, Into.Radius = Round.Radius, Into.Focus = Round.Focus;
        return TakeStops(From, Head.Stops, Into.Stops);
    }
    case ITEM_FILL_CONIC: {
        ItemConic Turn{};
        if (!From.Take(Turn) || !Within(Turn.Middle)) return false;
        Into.From = Turn.Middle, Into.Start = Turn.Start;
        return TakeStops(From, Head.Stops, Into.Stops);
    }
    case ITEM_FILL_PICTURE: {
        ItemPictureFill Picture{};
        if (!From.Take(Picture) || Picture.Block == 0 || !Within(Picture.At) || !Length(Picture.Size.X) ||
            !Length(Picture.Size.Y))
            return false;
        Into.Block = Picture.Block, Into.At = Picture.At, Into.Size = Picture.Size, Into.Sharp = Picture.Sharp != 0;
        return true;
    }
    default:
        return false;
    }
}

bool TakeStyle(Reader &From, TextStyle &Into)
{
    ItemTextStyle Head{};
    if (!From.Take(Head)) return false;
    if (Head.Said >= 512 || Head.FamilyBytes > 1024) return false;
    if ((Head.Said & ITEM_SAYS_SIZE) && (Head.Size <= 0 || Head.Size > 8192 * ITEM_POINT)) return false;
    if ((Head.Said & ITEM_SAYS_WEIGHT) && (Head.Weight < 1 || Head.Weight > 1000)) return false;
    if ((Head.Said & ITEM_SAYS_SPACING) && !Within(Head.Spacing)) return false;
    if ((Head.Said & ITEM_SAYS_LINE_HEIGHT) && (Head.LineHeight < 0 || Head.LineHeight > 64 * ITEM_ONE)) return false;
    Into.Said       = Head.Said;
    Into.Size       = Head.Size;
    Into.Weight     = Head.Weight;
    Into.Italic     = Head.Italic != 0;
    Into.Spacing    = Head.Spacing;
    Into.LineHeight = Head.LineHeight;
    Into.Underline  = Head.Underline != 0;
    Into.Strike     = Head.Strike != 0;
    Into.Color      = Head.Color;
    return From.TakeText(Head.FamilyBytes, Into.Family);
}

bool TakeShape(Reader &From, Shape &Into)
{
    ItemShape Head{};
    if (!From.Take(Head) || Head.Form > ITEM_FORM_PATH || Head.Rule > 1) return false;
    Into.Form = Head.Form;
    Into.Rule = Head.Rule;
    if (Head.Form != ITEM_FORM_PATH) return Head.Steps == 0;
    if (Head.Steps > ITEM_PATH_MOST) return false;
    Into.Steps.resize(Head.Steps);
    std::size_t Points = 0;
    for (std::uint32_t I = 0; I < Head.Steps; I++) {
        if (!From.Take(Into.Steps[I])) return false;
        switch (Into.Steps[I]) {
        case ITEM_STEP_MOVE: case ITEM_STEP_LINE: Points += 1; break;
        case ITEM_STEP_QUAD: Points += 2; break;
        case ITEM_STEP_CUBIC: Points += 3; break;
        case ITEM_STEP_CLOSE: break;
        default: return false;
        }
    }
    for (std::uint32_t Pad = Head.Steps; Pad % 4; Pad++) {
        std::uint8_t Skip;
        if (!From.Take(Skip)) return false;
    }
    Into.Points.resize(Points);
    for (ItemPoint &Each : Into.Points)
        if (!From.Take(Each) || !Within(Each)) return false;
    return true;
}

bool TakeStroke(Reader &From, Stroke &Into)
{
    ItemStroke Head{};
    if (!From.Take(Head) || !Length(Head.Width) || Head.Cap > ITEM_CAP_SQUARE || Head.Join > ITEM_JOIN_BEVEL ||
        Head.Align > ITEM_ALIGN_OUTSIDE || Head.Dashes > ITEM_DASHES_MOST || Head.MiterLimit < ITEM_POINT ||
        Head.MiterLimit > 1024 * ITEM_POINT || !Within(Head.DashOffset))
        return false;
    Into.Width      = Head.Width;
    Into.Cap        = Head.Cap;
    Into.Join       = Head.Join;
    Into.Align      = Head.Align;
    Into.MiterLimit = Head.MiterLimit;
    Into.DashOffset = Head.DashOffset;
    Into.Dashes.resize(Head.Dashes);
    std::int64_t Period = 0;
    for (std::int32_t &Each : Into.Dashes) {
        if (!From.Take(Each) || !Length(Each)) return false;
        Period += Each;
    }
    if (Head.Dashes && Period == 0) return false;
    return TakeFill(From, Into.With);
}

bool TakeFills(Reader &From, std::vector<Fill> &Into)
{
    ItemFills Head{};
    if (!From.Take(Head) || Head.Count > ITEM_FILLS_MOST) return false;
    Into.resize(Head.Count);
    for (Fill &Each : Into)
        if (!TakeFill(From, Each)) return false;
    return true;
}

bool TakeShadows(Reader &From, std::vector<Shadow> &Into)
{
    ItemShadows Head{};
    if (!From.Take(Head) || Head.Count > ITEM_SHADOWS_MOST) return false;
    Into.resize(Head.Count);
    for (Shadow &Each : Into) {
        ItemShadow Wire{};
        if (!From.Take(Wire) || !Within(Wire.Offset) || !Length(Wire.Blur) || !Within(Wire.Spread)) return false;
        Each = Shadow{Wire.Offset, Wire.Blur, Wire.Spread, Wire.Color, Wire.Inner != 0};
    }
    return true;
}

bool TakeSpans(Reader &From, std::vector<Span> &Into)
{
    ItemSpans Head{};
    if (!From.Take(Head) || Head.Count > ITEM_SPANS_MOST) return false;
    Into.resize(Head.Count);
    for (Span &Each : Into) {
        ItemSpanRange Range{};
        if (!From.Take(Range) || Range.From > Range.To || !TakeStyle(From, Each.Style)) return false;
        Each.From = Range.From, Each.To = Range.To;
    }
    return true;
}

// The value of Property read into Said: true when it reads whole.
bool Read(std::uint16_t Property, Reader &From, Properties &Said, std::uint16_t &Role)
{
    switch (Property) {
    case ITEM_PROP_PLACE: {
        ItemPoint Point{};
        if (!From.Take(Point) || !Within(Point)) return false;
        return Said.Place = Point, true;
    }
    case ITEM_PROP_SIZE: {
        ItemPoint Point{};
        if (!From.Take(Point)) return false;
        if ((Point.X != ITEM_BY_CONTENT && !Length(Point.X)) || (Point.Y != ITEM_BY_CONTENT && !Length(Point.Y)))
            return false;
        return Said.Size = Point, true;
    }
    case ITEM_PROP_OPACITY: {
        ItemOpacity Opacity{};
        if (!From.Take(Opacity)) return false;
        return Said.Opacity = Opacity.Opacity, true;
    }
    case ITEM_PROP_HIDDEN:
    case ITEM_PROP_CLIP:
    case ITEM_PROP_SECRET: {
        ItemFlag Flag{};
        if (!From.Take(Flag) || Flag.On > 1) return false;
        std::optional<bool> &Into = Property == ITEM_PROP_HIDDEN ? Said.Hidden : Property == ITEM_PROP_CLIP ? Said.Clip : Said.Secret;
        return Into = Flag.On != 0, true;
    }
    case ITEM_PROP_TURN: {
        ItemTurn Turn{};
        if (!From.Take(Turn)) return false;
        return Said.Turn = Turn.Turn, true;
    }
    case ITEM_PROP_SCALE: {
        ItemPoint Scale{};
        if (!From.Take(Scale) || Scale.X < -64 * ITEM_ONE || Scale.X > 64 * ITEM_ONE || Scale.Y < -64 * ITEM_ONE ||
            Scale.Y > 64 * ITEM_ONE)
            return false;
        return Said.Scale = Scale, true;
    }
    case ITEM_PROP_ANCHOR: {
        ItemPoint Point{};
        if (!From.Take(Point) || !Within(Point)) return false;
        return Said.Anchor = Point, true;
    }
    case ITEM_PROP_CORNERS: {
        ItemCorners Corners{};
        if (!From.Take(Corners) || !Length(Corners.TopLeft) || !Length(Corners.TopRight) ||
            !Length(Corners.BottomRight) || !Length(Corners.BottomLeft))
            return false;
        return Said.Corners = std::array<std::int32_t, 4>{Corners.TopLeft, Corners.TopRight, Corners.BottomRight,
                                                           Corners.BottomLeft},
               true;
    }
    case ITEM_PROP_SHAPE: {
        Shape Made;
        if (!TakeShape(From, Made)) return false;
        return Said.Shape = std::move(Made), true;
    }
    case ITEM_PROP_FILLS: {
        std::vector<Fill> Made;
        if (!TakeFills(From, Made)) return false;
        return Said.Fills = std::move(Made), true;
    }
    case ITEM_PROP_STROKE: {
        Stroke Made;
        if (!TakeStroke(From, Made)) return false;
        return Said.Stroke = std::move(Made), true;
    }
    case ITEM_PROP_SHADOWS: {
        std::vector<Shadow> Made;
        if (!TakeShadows(From, Made)) return false;
        return Said.Shadows = std::move(Made), true;
    }
    case ITEM_PROP_TEXT:
    case ITEM_PROP_CAPTION: {
        ItemBytes Head{};
        std::string Made;
        if (!From.Take(Head) || Head.Bytes > ITEM_TEXT_MOST || !From.TakeText(Head.Bytes, Made)) return false;
        (Property == ITEM_PROP_TEXT ? Said.Text : Said.Caption) = std::move(Made);
        return true;
    }
    case ITEM_PROP_TEXT_STYLE: {
        TextStyle Made;
        if (!TakeStyle(From, Made)) return false;
        return Said.Style = std::move(Made), true;
    }
    case ITEM_PROP_TEXT_SPANS: {
        std::vector<Span> Made;
        if (!TakeSpans(From, Made)) return false;
        return Said.Spans = std::move(Made), true;
    }
    case ITEM_PROP_TEXT_BOX: {
        ItemTextBox Box{};
        if (!From.Take(Box) || Box.Align > ITEM_TEXT_JUSTIFY || Box.Overflow > ITEM_OVERFLOW_ELLIPSIS || Box.Wrap > 1)
            return false;
        return Said.Box = Box, true;
    }
    case ITEM_PROP_PICTURE: {
        ItemPictureOf Of{};
        if (!From.Take(Of) || Of.Block == 0 || Of.Fit > ITEM_FIT_CENTER || Of.Sharp > 1) return false;
        return Said.Picture = PictureOf{Of.Block, Of.Fit, Of.Sharp != 0}, true;
    }
    case ITEM_PROP_CANVAS: {
        ItemBlockOf Of{};
        if (!From.Take(Of) || Of.Block == 0) return false;
        return Said.Canvas = Of.Block, true;
    }
    case ITEM_PROP_ROLE: {
        ItemRoleOf Of{};
        if (!From.Take(Of) || Of.Role >= ITEM_ROLES) return false;
        return Role = Of.Role, true;
    }
    case ITEM_PROP_GRID: {
        ItemGrid Grid{};
        if (!From.Take(Grid) || Grid.Cols == 0 || Grid.Rows == 0 || Grid.Cols > ITEM_GRID_SIDE_MOST ||
            Grid.Rows > ITEM_GRID_SIDE_MOST)
            return false;
        return Said.Grid = Grid, true;
    }
    case ITEM_PROP_CARET: {
        ItemCaret Caret{};
        if (!From.Take(Caret) || Caret.Shown > 1) return false;
        return Said.Caret = Caret, true;
    }
    default:
        return false;
    }
}

// Property's value moved from From into Into.
void Take(std::uint16_t Property, Properties &From, Properties &Into) noexcept
{
    switch (Property) {
    case ITEM_PROP_PLACE: Into.Place = From.Place; break;
    case ITEM_PROP_SIZE: Into.Size = From.Size; break;
    case ITEM_PROP_OPACITY: Into.Opacity = From.Opacity; break;
    case ITEM_PROP_HIDDEN: Into.Hidden = From.Hidden; break;
    case ITEM_PROP_TURN: Into.Turn = From.Turn; break;
    case ITEM_PROP_SCALE: Into.Scale = From.Scale; break;
    case ITEM_PROP_ANCHOR: Into.Anchor = From.Anchor; break;
    case ITEM_PROP_CLIP: Into.Clip = From.Clip; break;
    case ITEM_PROP_CORNERS: Into.Corners = From.Corners; break;
    case ITEM_PROP_SHAPE: Into.Shape = std::move(From.Shape); break;
    case ITEM_PROP_FILLS: Into.Fills = std::move(From.Fills); break;
    case ITEM_PROP_STROKE: Into.Stroke = std::move(From.Stroke); break;
    case ITEM_PROP_SHADOWS: Into.Shadows = std::move(From.Shadows); break;
    case ITEM_PROP_TEXT: Into.Text = std::move(From.Text); break;
    case ITEM_PROP_TEXT_STYLE: Into.Style = std::move(From.Style); break;
    case ITEM_PROP_TEXT_SPANS: Into.Spans = std::move(From.Spans); break;
    case ITEM_PROP_TEXT_BOX: Into.Box = From.Box; break;
    case ITEM_PROP_SECRET: Into.Secret = From.Secret; break;
    case ITEM_PROP_CAPTION: Into.Caption = std::move(From.Caption); break;
    case ITEM_PROP_PICTURE: Into.Picture = From.Picture; break;
    case ITEM_PROP_CANVAS: Into.Canvas = From.Canvas; break;
    case ITEM_PROP_GRID: Into.Grid = From.Grid; break;
    case ITEM_PROP_CARET: Into.Caret = From.Caret; break;
    default: break;
    }
}

}  // namespace

error_t Decode(std::uint16_t Property, std::span<const std::uint8_t> Bytes, Properties &Into, std::uint16_t &Role) noexcept
{
    if (Property == 0 || Property >= ITEM_PROPS) return ERR_UNSUPPORTED;
    try {
        // Read into a value of its own, so a value that breaks leaves the
        // property as it was, and the others are never copied.
        Properties    Fresh;
        std::uint16_t Said = Role;
        Reader        From(Bytes);
        if (!Read(Property, From, Fresh, Said) || !From.Whole()) return ERR_INVALID_ARGUMENT;
        Take(Property, Fresh, Into);
        Role = Said;
        return OK;
    } catch (...) {
        return ERR_NO_MEMORY;
    }
}

error_t Unsay(std::uint16_t Property, Properties &Into) noexcept
{
    switch (Property) {
    case ITEM_PROP_PLACE: Into.Place.reset(); break;
    case ITEM_PROP_SIZE: Into.Size.reset(); break;
    case ITEM_PROP_OPACITY: Into.Opacity.reset(); break;
    case ITEM_PROP_HIDDEN: Into.Hidden.reset(); break;
    case ITEM_PROP_TURN: Into.Turn.reset(); break;
    case ITEM_PROP_SCALE: Into.Scale.reset(); break;
    case ITEM_PROP_ANCHOR: Into.Anchor.reset(); break;
    case ITEM_PROP_CLIP: Into.Clip.reset(); break;
    case ITEM_PROP_CORNERS: Into.Corners.reset(); break;
    case ITEM_PROP_SHAPE: Into.Shape.reset(); break;
    case ITEM_PROP_FILLS: Into.Fills.reset(); break;
    case ITEM_PROP_STROKE: Into.Stroke.reset(); break;
    case ITEM_PROP_SHADOWS: Into.Shadows.reset(); break;
    case ITEM_PROP_TEXT: Into.Text.reset(); break;
    case ITEM_PROP_TEXT_STYLE: Into.Style.reset(); break;
    case ITEM_PROP_TEXT_SPANS: Into.Spans.reset(); break;
    case ITEM_PROP_TEXT_BOX: Into.Box.reset(); break;
    case ITEM_PROP_SECRET: Into.Secret.reset(); break;
    case ITEM_PROP_CAPTION: Into.Caption.reset(); break;
    case ITEM_PROP_PICTURE: Into.Picture.reset(); break;
    case ITEM_PROP_CANVAS: Into.Canvas.reset(); break;
    case ITEM_PROP_GRID: Into.Grid.reset(); break;
    case ITEM_PROP_CARET: Into.Caret.reset(); break;
    case ITEM_PROP_ROLE: break;
    default: return ERR_UNSUPPORTED;
    }
    return OK;
}

std::size_t TextBytes(const Properties &Of) noexcept
{
    std::size_t Total = (Of.Text ? Of.Text->size() : 0) + (Of.Caption ? Of.Caption->size() : 0) +
                        (Of.Style ? Of.Style->Family.size() : 0);
    if (Of.Spans)
        for (const Span &Each : *Of.Spans) Total += Each.Style.Family.size() + sizeof(Span);
    return Total;
}

}  // namespace items
