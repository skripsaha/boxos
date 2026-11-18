/*
 * look.cpp — how an item looks: said, or its role's. See painter/items/look.h.
 */

#include "painter/items/look.h"

#include <algorithm>

namespace painter::items {

namespace {

constexpr std::int32_t Pt(std::int32_t Points) noexcept { return Points * ITEM_POINT; }

Fill ThemeFill(std::uint16_t Which, std::uint16_t Opacity = ITEM_OPAQUE)
{
    Fill Made;
    Made.Kind    = ITEM_FILL_THEME;
    Made.Theme   = Which;
    Made.Opacity = Opacity;
    return Made;
}

ItemColor Gray(std::uint16_t Alpha) noexcept { return ItemColor{0, 0, 0, Alpha}; }

}  // namespace

Theme ThemeOf(std::uint32_t TextColor, std::uint32_t PageColor, const ThemeSaid &Person)
{
    using painter::core::Color;
    Theme Made;
    Made.Text    = Color::FromRaw(TextColor);
    Made.Page    = Color::FromRaw(PageColor);
    Made.Panel   = Color::Over(Made.Page, Made.Text, 20);
    Made.Quiet   = Color::Over(Made.Page, Made.Text, 150);
    Made.Accent  = Person.Accent;
    Made.Round   = std::max(0, Person.Round);
    Made.Shadows = Person.Shadows;
    Made.Insist  = Person.Insist;
    /* Words on the accent dark on a light one, light on a dark one. */
    const painter::core::Oklab Seen = painter::core::OklabOf(Made.Accent.Red() / 255.0f, Made.Accent.Green() / 255.0f, Made.Accent.Blue() / 255.0f);
    Made.OnAccent = Seen.L > 0.72f ? Color::Rgb(0x14, 0x14, 0x14) : Color::Rgb(0xFF, 0xFF, 0xFF);
    return Made;
}

painter::core::Color ColorOf(const ItemColor &Of) noexcept
{
    const auto Eight = [](std::uint16_t Sixteen) { return static_cast<std::uint8_t>((Sixteen + 128u) / 257u); };
    return painter::core::Color::Rgba(Eight(Of.Red), Eight(Of.Green), Eight(Of.Blue), Eight(Of.Alpha));
}

painter::core::Color ThemeColor(const Theme &Of, std::uint16_t Which) noexcept
{
    switch (Which) {
    case ITEM_THEME_PAGE: return Of.Page;
    case ITEM_THEME_PANEL: return Of.Panel;
    case ITEM_THEME_ACCENT: return Of.Accent;
    case ITEM_THEME_QUIET: return Of.Quiet;
    case ITEM_THEME_ON_ACCENT: return Of.OnAccent;
    default: return Of.Text;
    }
}

namespace {

/* Whether the theme's look stands over what an item says: where the person
 * insists, for every item with a role. */
bool Insisted(std::uint16_t Role, const Theme &With) noexcept { return With.Insist && Role != ITEM_ROLE_NONE; }

}  // namespace

std::vector<Fill> FillsOf(const Properties &Said, std::uint8_t Kind, std::uint16_t Role, const Theme &With)
{
    if (Said.Fills && !Insisted(Role, With)) return *Said.Fills;
    if (Kind == ITEM_TEXT)
        return {ThemeFill(Role == ITEM_ROLE_HINT ? ITEM_THEME_QUIET : Role == ITEM_ROLE_BUTTON ? ITEM_THEME_ON_ACCENT : ITEM_THEME_TEXT)};
    switch (Role) {
    case ITEM_ROLE_PAGE: return {ThemeFill(ITEM_THEME_PAGE)};
    case ITEM_ROLE_PANEL:
    case ITEM_ROLE_FIELD: return {ThemeFill(ITEM_THEME_PANEL)};
    case ITEM_ROLE_BUTTON: return {ThemeFill(ITEM_THEME_ACCENT)};
    case ITEM_ROLE_SELECTION: return {ThemeFill(ITEM_THEME_ACCENT, 22000)};
    case ITEM_ROLE_ICON: return {ThemeFill(ITEM_THEME_TEXT)};
    case ITEM_ROLE_SEPARATOR: return {ThemeFill(ITEM_THEME_QUIET, 30000)};
    default: return {};
    }
}

std::array<std::int32_t, 4> CornersOf(const Properties &Said, std::uint16_t Role, const Theme &With) noexcept
{
    if (Said.Corners && !Insisted(Role, With)) return *Said.Corners;
    /* A button as round as the theme says; a panel half again, a field three
     * quarters, a selection three eighths. */
    const std::int32_t Round = Role == ITEM_ROLE_PANEL ? With.Round * 3 / 2 : Role == ITEM_ROLE_BUTTON ? With.Round
                             : Role == ITEM_ROLE_FIELD ? With.Round * 3 / 4 : Role == ITEM_ROLE_SELECTION ? With.Round * 3 / 8 : 0;
    return {Round, Round, Round, Round};
}

std::vector<Shadow> ShadowsOf(const Properties &Said, std::uint16_t Role, const Theme &With)
{
    if (Said.Shadows && !Insisted(Role, With)) return *Said.Shadows;
    if (!With.Shadows) return {};
    if (Role == ITEM_ROLE_PANEL) return {Shadow{{0, Pt(4)}, Pt(18), 0, Gray(26000), false}};
    if (Role == ITEM_ROLE_BUTTON) return {Shadow{{0, Pt(1)}, Pt(4), 0, Gray(20000), false}};
    if (Role == ITEM_ROLE_FIELD) return {Shadow{{0, Pt(1)}, Pt(3), 0, Gray(30000), true}};
    return {};
}

bool StrokeOf(const Properties &Said, std::uint16_t Role, const Theme &With, Stroke &Into)
{
    if (Said.Stroke && !Insisted(Role, With)) {
        Into = *Said.Stroke;
        return Into.Width > 0;
    }
    if (Role != ITEM_ROLE_FIELD) return false;
    Into       = Stroke{};
    Into.Width = Pt(1);
    Into.Align = ITEM_ALIGN_INSIDE;
    Into.With  = ThemeFill(ITEM_THEME_QUIET, 40000);
    return true;
}

TextStyle StyleOf(const Properties &Said, std::uint16_t Role, const Theme &With)
{
    TextStyle Made;
    Made.Said   = static_cast<std::uint16_t>(0x1FF & ~ITEM_SAYS_COLOR);
    Made.Family = Role == ITEM_ROLE_CODE ? With.CodeFamily : With.TextFamily;
    Made.Size   = Role == ITEM_ROLE_HEADING ? Pt(22) : Role == ITEM_ROLE_CODE || Role == ITEM_ROLE_LABEL ? Pt(14)
                : Role == ITEM_ROLE_HINT ? Pt(13) : Pt(15);
    Made.Weight = Role == ITEM_ROLE_HEADING || Role == ITEM_ROLE_BUTTON ? 600 : Role == ITEM_ROLE_LABEL ? 500 : 400;
    if (!Said.Style) return Made;
    const TextStyle &Own = *Said.Style;
    if (Own.Said & ITEM_SAYS_FAMILY) Made.Family = Own.Family;
    if (Own.Said & ITEM_SAYS_SIZE) Made.Size = Own.Size;
    if (Own.Said & ITEM_SAYS_WEIGHT) Made.Weight = Own.Weight;
    if (Own.Said & ITEM_SAYS_ITALIC) Made.Italic = Own.Italic;
    if (Own.Said & ITEM_SAYS_SPACING) Made.Spacing = Own.Spacing;
    if (Own.Said & ITEM_SAYS_LINE_HEIGHT) Made.LineHeight = Own.LineHeight;
    if (Own.Said & ITEM_SAYS_UNDERLINE) Made.Underline = Own.Underline;
    if (Own.Said & ITEM_SAYS_STRIKE) Made.Strike = Own.Strike;
    if (Own.Said & ITEM_SAYS_COLOR) Made.Color = Own.Color, Made.Said |= ITEM_SAYS_COLOR;
    return Made;
}

bool Plated(std::uint8_t Kind, std::uint16_t Role) noexcept
{
    return Kind == ITEM_TEXT && (Role == ITEM_ROLE_BUTTON || Role == ITEM_ROLE_FIELD);
}

std::vector<Fill> PlateOf(std::uint16_t Role)
{
    return {ThemeFill(Role == ITEM_ROLE_BUTTON ? ITEM_THEME_ACCENT : ITEM_THEME_PANEL)};
}

ItemPoint InsetOf(std::uint16_t Role) noexcept
{
    if (Role == ITEM_ROLE_BUTTON) return ItemPoint{Pt(14), Pt(7)};
    if (Role == ITEM_ROLE_FIELD) return ItemPoint{Pt(10), Pt(7)};
    return ItemPoint{0, 0};
}

bool FocusableOf(const Properties &Said, std::uint8_t Kind, std::uint16_t Role) noexcept
{
    if (Said.Focusable) return *Said.Focusable;
    (void)Kind;
    return Role == ITEM_ROLE_BUTTON || Role == ITEM_ROLE_FIELD;
}

bool DisabledOf(const Properties &Said) noexcept { return Said.Disabled && *Said.Disabled; }

std::uint16_t OpacityOf(const Properties &Said) noexcept { return Said.Opacity ? *Said.Opacity : ITEM_OPAQUE; }

bool HiddenOf(const Properties &Said) noexcept { return Said.Hidden && *Said.Hidden; }

}  // namespace painter::items
