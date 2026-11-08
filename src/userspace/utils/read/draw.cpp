/* The few ways read draws beside the page itself. */

#include "draw.h"

#include <algorithm>
#include <format>

#include "glyph.h"

namespace read {

namespace {

/* How far toward black a shadow takes what lies under it, of 255. */
constexpr std::uint8_t ShadowDepth = 72;

/* The cell at At darkened, its glyph kept: the window's shadow. */
void Shade(tui::Frame &Out, core::Point At)
{
    tui::Cell Under  = Out.Read(At);
    Under.Foreground = core::Color::Mix(Under.Foreground, core::Colors::Black, ShadowDepth);
    Under.Background = core::Color::Mix(Under.Background, core::Colors::Black, ShadowDepth);
    (void)Out.Put(At, Under);
}

}  // namespace

std::int32_t PutText(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Paper)
{
    std::int32_t Col = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte);
        if (Col + static_cast<std::int32_t>(Shape.Width) > Width) break;
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++)
            (void)Out.Put({At.X + Col++, At.Y}, tui::Cell{Shape.Cells[Cell], Ink, Paper});
        Byte += Shape.Length;
    }
    return Col;
}

std::string Shortened(std::string_view Text, std::size_t Width)
{
    if (CellsOf(Text) <= Width) return std::string(Text);
    if (Width <= 3) return std::string(Width, '.');
    return std::string(Text.substr(0, BytesWithin(Text, Width - 3))) + "...";
}

core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept
{
    Width  = std::clamp(Width, 1, std::max(1, Screen.Width));
    Height = std::clamp(Height, 1, std::max(1, Screen.Height));
    return {(Screen.Width - Width) / 2, (Screen.Height - Height) / 2, Width, Height};
}

core::Rect DrawWindow(tui::Frame &Out, core::Rect Area, const Theme &Look, std::string_view Title,
                      std::string_view Hint)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t X = Area.X + 1; X <= Right + 1; X++) Shade(Out, {X, Bottom + 1});
    for (std::int32_t Y = Area.Y + 1; Y <= Bottom; Y++) Shade(Out, {Right + 1, Y});

    const tui::TextStyle Rule{Look.WindowRule, Look.WindowPaper};
    Out.Fill(Area, tui::Cell{' ', Look.Ink, Look.WindowPaper});
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        (void)Out.Put({X, Area.Y}, RuleAcross, Rule);
        (void)Out.Put({X, Bottom}, RuleAcross, Rule);
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        (void)Out.Put({Area.X, Y}, RuleDown, Rule);
        (void)Out.Put({Right, Y}, RuleDown, Rule);
    }
    (void)Out.Put({Area.X, Area.Y}, CornerTL, Rule);
    (void)Out.Put({Right, Area.Y}, CornerTR, Rule);
    (void)Out.Put({Area.X, Bottom}, CornerBL, Rule);
    (void)Out.Put({Right, Bottom}, CornerBR, Rule);

    const std::int32_t Room = Area.Width - 4;
    if (!Title.empty()) PutText(Out, {Area.X + 2, Area.Y}, std::format(" {} ", Title), Room, Look.Heading, Look.WindowPaper);
    if (!Hint.empty()) PutText(Out, {Area.X + 2, Bottom}, std::format(" {} ", Hint), Room, Look.Faint, Look.WindowPaper);
    return {Area.X + 2, Area.Y + 1, Area.Width - 4, Area.Height - 2};
}

}  // namespace read
