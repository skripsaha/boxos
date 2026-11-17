/* window.cpp — a window with a frame and a shadow. */

#include "painter/tui/window.h"

#include <algorithm>
#include <format>
#include <string>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

namespace painter::tui {

namespace {

/* How far a window's shadow darkens what lies under it, of 255, laid as light
 * (Color::Over): enough to lift the window off the text, little enough that
 * the text under it still reads. */
constexpr std::uint8_t ShadowDepth = 103;

void Shade(Frame &Out, core::Point At)
{
    Cell Under       = Out.Read(At);
    Under.Foreground = core::Color::Over(Under.Foreground, core::Colors::Black, ShadowDepth);
    Under.Background = core::Color::Over(Under.Background, core::Colors::Black, ShadowDepth);
    (void)Out.Put(At, Under);
}

}  // namespace

core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept
{
    Width  = std::clamp(Width, 1, std::max(1, Screen.Width));
    Height = std::clamp(Height, 1, std::max(1, Screen.Height));
    return {(Screen.Width - Width) / 2, (Screen.Height - Height) / 2, Width, Height};
}

core::Rect DrawWindow(Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note,
                      std::string_view Hint)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t X = Area.X + 1; X <= Right + 1; X++) Shade(Out, {X, Bottom + 1});
    for (std::int32_t Y = Area.Y + 1; Y <= Bottom; Y++) Shade(Out, {Right + 1, Y});

    Out.Fill(Area, Cell{' ', WindowInk, WindowPaper});
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        (void)Out.Put({X, Area.Y}, Glyphs::RuleAcross, {WindowRule, WindowPaper});
        (void)Out.Put({X, Bottom}, Glyphs::RuleAcross, {WindowRule, WindowPaper});
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        (void)Out.Put({Area.X, Y}, Glyphs::RuleDown, {WindowRule, WindowPaper});
        (void)Out.Put({Right, Y}, Glyphs::RuleDown, {WindowRule, WindowPaper});
    }
    (void)Out.Put({Area.X, Area.Y}, Glyphs::CornerTL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Area.Y}, Glyphs::CornerTR, {WindowRule, WindowPaper});
    (void)Out.Put({Area.X, Bottom}, Glyphs::CornerBL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Bottom}, Glyphs::CornerBR, {WindowRule, WindowPaper});

    const std::int32_t Room = Area.Width - 4;
    std::int32_t       Used = 0;
    if (!Title.empty()) Used = PutText(Out, {Area.X + 2, Area.Y}, std::format(" {} ", Title), Room, TitleInk, WindowPaper);
    if (!Note.empty()) {
        const std::string  Said  = std::format(" {} ", Note);
        const std::int32_t Cells = static_cast<std::int32_t>(CellsOf(Said));
        if (Cells <= Room - Used - 1) PutText(Out, {Right - 1 - Cells, Area.Y}, Said, Cells, HintInk, WindowPaper);
    }
    if (!Hint.empty()) PutText(Out, {Area.X + 2, Bottom}, std::format(" {} ", Hint), Room, HintInk, WindowPaper);
    return {Area.X + 2, Area.Y + 1, Area.Width - 4, Area.Height - 2};
}

void DrawRule(Frame &Out, core::Rect Inside, std::int32_t Row)
{
    const std::int32_t Left = Inside.X - 2, Right = Inside.X + Inside.Width + 1;
    for (std::int32_t X = Left + 1; X < Right; X++) (void)Out.Put({X, Row}, Glyphs::RuleAcross, {WindowRule, WindowPaper});
    (void)Out.Put({Left, Row}, Glyphs::TeeLeft, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Row}, Glyphs::TeeRight, {WindowRule, WindowPaper});
}

}  // namespace painter::tui
