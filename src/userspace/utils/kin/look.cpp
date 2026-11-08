/* The few ways edit and read draw alike. */

#include "look.h"

#include <algorithm>
#include <format>

#include "glyph.h"

namespace kin {

namespace {

/* How far a window's shadow darkens what lies under it, of 255: enough to lift
 * the window off the text, little enough that the text under it still reads. */
constexpr std::uint8_t ShadowDepth = 72;

void Shade(tui::Frame &Out, core::Point At)
{
    tui::Cell Under  = Out.Read(At);
    Under.Foreground = core::Color::Mix(Under.Foreground, core::Colors::Black, ShadowDepth);
    Under.Background = core::Color::Mix(Under.Background, core::Colors::Black, ShadowDepth);
    (void)Out.Put(At, Under);
}

}  // namespace

std::string SizeText(std::uint64_t Bytes)
{
    if (Bytes < 1024) return std::format("{} B", Bytes);
    if (Bytes < 1024 * 1024) return std::format("{} KB", (Bytes + 1023) / 1024);
    return std::format("{}.{} MB", Bytes / (1024 * 1024), Bytes % (1024 * 1024) * 10 / (1024 * 1024));
}

std::string Shortened(std::string_view Text, std::size_t Width)
{
    if (CellsOf(Text) <= Width) return std::string(Text);
    if (Width <= 3) return std::string(Width, '.');
    return std::string(Text.substr(0, BytesWithin(Text, Width - 3))) + "...";
}

std::int32_t PutText(tui::Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Under)
{
    std::int32_t Col = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte, static_cast<std::size_t>(Col));
        if (Col + static_cast<std::int32_t>(Shape.Width) > Width) break;
        const bool Blank = Text[Byte] == '\t';
        for (std::size_t Cell = 0; Cell < Shape.Width; Cell++)
            (void)Out.Put({At.X + Col++, At.Y},
                          tui::Cell{Blank ? ' ' : Shape.Cells[Cell], Shape.Odd ? OddInk : Ink, Under});
        Byte += Shape.Length;
    }
    return Col;
}

core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept
{
    Width  = std::clamp(Width, 1, std::max(1, Screen.Width));
    Height = std::clamp(Height, 1, std::max(1, Screen.Height));
    return {(Screen.Width - Width) / 2, (Screen.Height - Height) / 2, Width, Height};
}

core::Rect DrawWindow(tui::Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note,
                      std::string_view Hint)
{
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t X = Area.X + 1; X <= Right + 1; X++) Shade(Out, {X, Bottom + 1});
    for (std::int32_t Y = Area.Y + 1; Y <= Bottom; Y++) Shade(Out, {Right + 1, Y});

    Out.Fill(Area, tui::Cell{' ', WindowInk, WindowPaper});
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        (void)Out.Put({X, Area.Y}, RuleAcross, {WindowRule, WindowPaper});
        (void)Out.Put({X, Bottom}, RuleAcross, {WindowRule, WindowPaper});
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        (void)Out.Put({Area.X, Y}, RuleDown, {WindowRule, WindowPaper});
        (void)Out.Put({Right, Y}, RuleDown, {WindowRule, WindowPaper});
    }
    (void)Out.Put({Area.X, Area.Y}, CornerTL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Area.Y}, CornerTR, {WindowRule, WindowPaper});
    (void)Out.Put({Area.X, Bottom}, CornerBL, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Bottom}, CornerBR, {WindowRule, WindowPaper});

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

void DrawRule(tui::Frame &Out, core::Rect Inside, std::int32_t Row)
{
    const std::int32_t Left = Inside.X - 2, Right = Inside.X + Inside.Width + 1;
    for (std::int32_t X = Left + 1; X < Right; X++) (void)Out.Put({X, Row}, RuleAcross, {WindowRule, WindowPaper});
    (void)Out.Put({Left, Row}, TeeLeft, {WindowRule, WindowPaper});
    (void)Out.Put({Right, Row}, TeeRight, {WindowRule, WindowPaper});
}

}  // namespace kin
