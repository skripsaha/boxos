/*
 * between.cpp — the values between two of a property. See
 * painter/items/between.h.
 */

#include "painter/items/between.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>

#include "painter/core/color.h"

namespace painter::items {

namespace {

/* Numbers and form, written one after another. */
struct Writer {
    Numbers &Into;
    void     Value(double Of) { Into.Values.push_back(Of); }
    void     Point(const ItemPoint &Of) { Value(Of.X), Value(Of.Y); }
    void     Form(std::uint32_t Of) { Into.Form.push_back(Of); }
    /* A colour as the eye sees it, weighed by its opacity, and the opacity. */
    void Color(const ItemColor &Of)
    {
        const painter::core::Oklab Lab = painter::core::OklabOf(Of.Red / 65535.0f, Of.Green / 65535.0f, Of.Blue / 65535.0f);
        const double               Alpha = Of.Alpha / 65535.0;
        Value(Lab.L * Alpha), Value(Lab.A * Alpha), Value(Lab.B * Alpha), Value(Alpha);
    }
};

/* Numbers read back one after another, in the order written. */
struct Reader {
    std::span<const double> From;
    std::size_t             At = 0;
    double        Value() { return At < From.size() ? From[At++] : 0.0; }
    std::int32_t  Whole() { return static_cast<std::int32_t>(std::llround(std::clamp(Value(), -2147483648.0, 2147483647.0))); }
    std::uint16_t Short() { return static_cast<std::uint16_t>(std::llround(std::clamp(Value(), 0.0, 65535.0))); }
    ItemPoint     Point()
    {
        const std::int32_t X = Whole();
        return ItemPoint{X, Whole()};
    }
    ItemColor Color()
    {
        const double L = Value(), A = Value(), B = Value(), Alpha = std::clamp(Value(), 0.0, 1.0);
        if (Alpha <= 0) return ItemColor{0, 0, 0, 0};
        float Red = 0, Green = 0, Blue = 0;
        painter::core::RgbOf(painter::core::Oklab{static_cast<float>(L / Alpha), static_cast<float>(A / Alpha),
                                                  static_cast<float>(B / Alpha)},
                             Red, Green, Blue);
        const auto Sixteen = [](float Of) { return static_cast<std::uint16_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 65535.0f)); };
        return ItemColor{Sixteen(Red), Sixteen(Green), Sixteen(Blue), static_cast<std::uint16_t>(std::llround(Alpha * 65535.0))};
    }
};

ItemColor ColorOfTheme(const Theme &Look, std::uint16_t Which) noexcept
{
    const painter::core::Color Of = ThemeColor(Look, Which);
    return ItemColor{static_cast<std::uint16_t>(Of.Red() * 257u), static_cast<std::uint16_t>(Of.Green() * 257u),
                     static_cast<std::uint16_t>(Of.Blue() * 257u), static_cast<std::uint16_t>(Of.Opacity() * 257u)};
}

/* A fill: a colour of the theme's walks as the colour it is. */
void WriteFill(Writer &To, const Fill &Of, const Theme &Look)
{
    const bool Themed = Of.Kind == ITEM_FILL_THEME;
    To.Form(Themed ? static_cast<std::uint32_t>(ITEM_FILL_COLOR) : Of.Kind), To.Form(Of.Extend), To.Form(static_cast<std::uint32_t>(Of.Stops.size()));
    To.Form(Of.Block), To.Form(Of.Sharp);
    To.Value(Of.Opacity);
    To.Color(Themed ? ColorOfTheme(Look, Of.Theme) : Of.Color);
    To.Point(Of.From), To.Point(Of.To), To.Value(Of.Radius), To.Point(Of.Focus), To.Value(Of.Start);
    for (const ItemStop &Stop : Of.Stops) To.Value(Stop.Offset), To.Color(Stop.Color);
    To.Point(Of.At), To.Point(Of.Size);
}

Fill ReadFill(Reader &From, const Fill &Like)
{
    Fill Made    = Like;
    Made.Kind    = Like.Kind == ITEM_FILL_THEME ? static_cast<std::uint8_t>(ITEM_FILL_COLOR) : Like.Kind;
    Made.Opacity = From.Short();
    Made.Color   = From.Color();
    Made.From = From.Point(), Made.To = From.Point(), Made.Radius = From.Whole(), Made.Focus = From.Point(), Made.Start = From.Whole();
    for (ItemStop &Stop : Made.Stops) Stop.Offset = From.Short(), Stop.Color = From.Color();
    Made.At = From.Point(), Made.Size = From.Point();
    return Made;
}

/* Those one list has more of than the other, added to the other gone. */
template <typename Each, typename Gone> void Pad(std::vector<Each> &Shorter, const std::vector<Each> &Longer, Gone &&Make)
{
    for (std::size_t I = Shorter.size(); I < Longer.size(); I++) Shorter.push_back(Make(Longer[I]));
}

}  // namespace

bool SayAsShown(std::uint16_t Property, std::uint8_t Kind, std::uint16_t Role, const Theme &Look, Properties &Said)
{
    switch (Property) {
    case ITEM_PROP_PLACE:
        if (!Said.Place) Said.Place = ItemPoint{0, 0};
        return true;
    case ITEM_PROP_ANCHOR:
        if (!Said.Anchor) Said.Anchor = ItemPoint{0, 0};
        return true;
    case ITEM_PROP_SCALE:
        if (!Said.Scale) Said.Scale = ItemPoint{ITEM_ONE, ITEM_ONE};
        return true;
    case ITEM_PROP_OPACITY:
        Said.Opacity = OpacityOf(Said);
        return true;
    case ITEM_PROP_TURN:
        if (!Said.Turn) Said.Turn = 0;
        return true;
    case ITEM_PROP_CORNERS:
        Said.Corners = CornersOf(Said, Role, Look);
        return true;
    case ITEM_PROP_SHAPE:
        if (!Said.Shape) Said.Shape = items::Shape{};
        return true;
    case ITEM_PROP_FILLS:
        Said.Fills = FillsOf(Said, Kind, Role, Look);
        return true;
    case ITEM_PROP_STROKE: {
        Stroke Line;
        if (StrokeOf(Said, Role, Look, Line)) Said.Stroke = Line;
        return true;
    }
    case ITEM_PROP_SHADOWS:
        Said.Shadows = ShadowsOf(Said, Role, Look);
        return true;
    case ITEM_PROP_TEXT_STYLE:
        Said.Style = StyleOf(Said, Role, Look);
        return true;
    default:
        return false;
    }
}

void MakeAlike(std::uint16_t Property, Properties &From, Properties &To)
{
    switch (Property) {
    case ITEM_PROP_SHADOWS: {
        if (!From.Shadows || !To.Shadows) return;
        const auto Gone = [](Shadow Of) {
            Of.Color.Alpha = 0;
            Of.Spread      = 0;
            return Of;
        };
        Pad(*From.Shadows, *To.Shadows, Gone);
        Pad(*To.Shadows, *From.Shadows, Gone);
        return;
    }
    case ITEM_PROP_FILLS: {
        if (!From.Fills || !To.Fills) return;
        const auto Gone = [](Fill Of) {
            Of.Opacity = 0;
            return Of;
        };
        Pad(*From.Fills, *To.Fills, Gone);
        Pad(*To.Fills, *From.Fills, Gone);
        return;
    }
    case ITEM_PROP_STROKE:
        if (From.Stroke.has_value() == To.Stroke.has_value()) return;
        if (!From.Stroke) From.Stroke = To.Stroke, From.Stroke->Width = 0;
        else To.Stroke = From.Stroke, To.Stroke->Width = 0;
        return;
    default:
        return;
    }
}

bool NumbersOf(std::uint16_t Property, const Properties &Of, const Theme &Look, Numbers &Into)
{
    Into = Numbers{};
    Writer To{Into};
    To.Form(Property);
    switch (Property) {
    case ITEM_PROP_PLACE:
    case ITEM_PROP_ANCHOR:
    case ITEM_PROP_SCALE:
    case ITEM_PROP_SIZE: {
        const std::optional<ItemPoint> &Point = Property == ITEM_PROP_PLACE    ? Of.Place
                                              : Property == ITEM_PROP_ANCHOR   ? Of.Anchor
                                              : Property == ITEM_PROP_SCALE    ? Of.Scale
                                                                               : Of.Size;
        if (!Point || Point->X == ITEM_BY_CONTENT || Point->Y == ITEM_BY_CONTENT) return false;
        To.Point(*Point);
        return true;
    }
    case ITEM_PROP_OPACITY:
        if (!Of.Opacity) return false;
        To.Value(*Of.Opacity);
        return true;
    case ITEM_PROP_TURN:
        if (!Of.Turn) return false;
        To.Value(*Of.Turn);
        return true;
    case ITEM_PROP_CORNERS:
        if (!Of.Corners) return false;
        for (std::int32_t Round : *Of.Corners) To.Value(Round);
        return true;
    case ITEM_PROP_SHAPE:
        if (!Of.Shape) return false;
        To.Form(Of.Shape->Form), To.Form(Of.Shape->Rule);
        for (std::uint8_t Step : Of.Shape->Steps) To.Form(Step);
        for (const ItemPoint &Point : Of.Shape->Points) To.Point(Point);
        return true;
    case ITEM_PROP_FILLS:
        if (!Of.Fills) return false;
        To.Form(static_cast<std::uint32_t>(Of.Fills->size()));
        for (const Fill &Each : *Of.Fills) WriteFill(To, Each, Look);
        return true;
    case ITEM_PROP_STROKE: {
        if (!Of.Stroke) return false;
        const Stroke &Line = *Of.Stroke;
        To.Form(Line.Cap), To.Form(Line.Join), To.Form(Line.Align), To.Form(static_cast<std::uint32_t>(Line.Dashes.size()));
        To.Value(Line.Width), To.Value(Line.MiterLimit), To.Value(Line.DashOffset);
        for (std::int32_t Dash : Line.Dashes) To.Value(Dash);
        WriteFill(To, Line.With, Look);
        return true;
    }
    case ITEM_PROP_SHADOWS:
        if (!Of.Shadows) return false;
        To.Form(static_cast<std::uint32_t>(Of.Shadows->size()));
        for (const Shadow &Each : *Of.Shadows) {
            To.Form(Each.Inner);
            To.Point(Each.Offset), To.Value(Each.Blur), To.Value(Each.Spread), To.Color(Each.Color);
        }
        return true;
    case ITEM_PROP_TEXT_STYLE: {
        if (!Of.Style) return false;
        const TextStyle &Style = *Of.Style;
        To.Form(Style.Said), To.Form(static_cast<std::uint32_t>(std::hash<std::string_view>{}(Style.Family)));
        To.Form(Style.Weight), To.Form(Style.Italic), To.Form(Style.Underline), To.Form(Style.Strike);
        To.Value(Style.Size), To.Value(Style.Spacing), To.Value(Style.LineHeight);
        if (Style.Said & ITEM_SAYS_COLOR) To.Color(Style.Color);
        return true;
    }
    default:
        return false;
    }
}

void SayNumbers(std::uint16_t Property, std::span<const double> Values, const Properties &Like, const Theme &Look,
                Properties &Into)
{
    (void)Look;
    Reader From{Values};
    switch (Property) {
    case ITEM_PROP_PLACE: Into.Place = From.Point(); return;
    case ITEM_PROP_ANCHOR: Into.Anchor = From.Point(); return;
    case ITEM_PROP_SCALE: Into.Scale = From.Point(); return;
    case ITEM_PROP_SIZE: Into.Size = From.Point(); return;
    case ITEM_PROP_OPACITY: Into.Opacity = From.Short(); return;
    case ITEM_PROP_TURN: Into.Turn = From.Whole(); return;
    case ITEM_PROP_CORNERS: {
        std::array<std::int32_t, 4> Round{};
        for (std::int32_t &Each : Round) Each = std::max(0, From.Whole());
        Into.Corners = Round;
        return;
    }
    case ITEM_PROP_SHAPE: {
        if (!Like.Shape) return;
        items::Shape Made = *Like.Shape;
        for (ItemPoint &Point : Made.Points) Point = From.Point();
        Into.Shape = std::move(Made);
        return;
    }
    case ITEM_PROP_FILLS: {
        if (!Like.Fills) return;
        std::vector<Fill> Made;
        Made.reserve(Like.Fills->size());
        for (const Fill &Each : *Like.Fills) Made.push_back(ReadFill(From, Each));
        Into.Fills = std::move(Made);
        return;
    }
    case ITEM_PROP_STROKE: {
        if (!Like.Stroke) return;
        Stroke Made     = *Like.Stroke;
        Made.Width      = std::max(0, From.Whole());
        Made.MiterLimit = From.Whole();
        Made.DashOffset = From.Whole();
        for (std::int32_t &Dash : Made.Dashes) Dash = std::max(0, From.Whole());
        Made.With   = ReadFill(From, Like.Stroke->With);
        Into.Stroke = std::move(Made);
        return;
    }
    case ITEM_PROP_SHADOWS: {
        if (!Like.Shadows) return;
        std::vector<Shadow> Made = *Like.Shadows;
        for (Shadow &Each : Made) {
            Each.Offset = From.Point(), Each.Blur = std::max(0, From.Whole()), Each.Spread = From.Whole();
            Each.Color  = From.Color();
        }
        Into.Shadows = std::move(Made);
        return;
    }
    case ITEM_PROP_TEXT_STYLE: {
        if (!Like.Style) return;
        TextStyle Made  = *Like.Style;
        Made.Size       = std::max(1, From.Whole());
        Made.Spacing    = From.Whole();
        Made.LineHeight = std::max(0, From.Whole());
        if (Made.Said & ITEM_SAYS_COLOR) Made.Color = From.Color();
        Into.Style = std::move(Made);
        return;
    }
    default:
        return;
    }
}

}  // namespace painter::items
