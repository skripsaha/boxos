// panel.cpp — the line at the bottom and the window of keys.

#include "panel.h"

#include <algorithm>
#include <format>
#include <utility>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

namespace orbits {

namespace {

namespace tui = painter::tui;
using painter::core::Rect;

const Color BarPaper = Color::Rgb(0x16, 0x18, 0x22);
const Color BarInk   = Color::Rgb(0xDA, 0xDC, 0xE6);
const Color KeyInk   = Color::Rgb(0xFF, 0xC8, 0x57);
const Color QuietInk = Color::Rgb(0x84, 0x88, 0xA0);

std::int32_t Cells(std::string_view Text)
{
    return static_cast<std::int32_t>(tui::CellsOf(Text));
}

// Text from X in Ink on the bar; the column after it.
std::int32_t Say(tui::Frame &Out, std::int32_t X, std::int32_t Row, std::int32_t Right, std::string_view Text,
                 Color Ink)
{
    if (X >= Right) return X;
    return X + tui::PutText(Out, {X, Row}, Text, Right - X, Ink, BarPaper);
}

std::string Mass(double Value)
{
    return Value < 1.0 ? std::format("{:.1f}", Value) : std::format("{:.0f}", Value);
}

// Keys, each with what it does quietly after it, as many as the bar holds.
void PaintKeys(tui::Frame &Out, std::int32_t Row, std::int32_t Right, const Readout &Said)
{
    const std::pair<std::string, std::string> Keys[] = {
        {"left right", "angle"}, {"\xE2\x86\x91\xE2\x86\x93", "power"}, {"space", "throw"},
        {"p", "pause"},          {"+ -", "time"},                       {"[ ]", "mass"},
        {"1-5", "skies"},        {Said.Spelled[KeyHelp], "keys"},       {"esc", "leave"}};
    std::int32_t X = 1;
    for (const auto &[Key, Does] : Keys) {
        if (X + Cells(Key) + 1 + Cells(Does) + 2 > Right) break;
        X = Say(Out, X, Row, Right, Key, KeyInk) + 1;
        X = Say(Out, X, Row, Right, Does, QuietInk) + 2;
    }
}

}  // namespace

void PaintBar(tui::Frame &Out, const Readout &Said)
{
    const painter::core::Size Size = Out.Size();
    const std::int32_t        Row  = Size.Height - 1;
    const std::int32_t        Right = Size.Width - 1;
    Out.Fill({0, Row, Size.Width, 1}, ' ', {BarInk, BarPaper});
    std::int32_t X = Say(Out, 1, Row, Right, "orbits", KeyInk) + 2;
    if (!Said.Note.empty()) {
        Say(Out, X, Row, Right, Said.Note, BarInk);
        return;
    }
    if (Said.KeysFirst) {
        PaintKeys(Out, Row, Right, Said);
        return;
    }
    X = Say(Out, X, Row, Right, Said.Preset, BarInk) + 2;
    X = Say(Out, X, Row, Right, std::format("{} bod{}", Said.Bodies, Said.Bodies == 1 ? "y" : "ies"), BarInk) + 2;
    if (Said.Merged > 0) X = Say(Out, X, Row, Right, std::format("merged {}", Said.Merged), QuietInk) + 2;
    if (Said.Escaped > 0) X = Say(Out, X, Row, Right, std::format("gone {}", Said.Escaped), QuietInk) + 2;
    if (Said.Paused) X = Say(Out, X, Row, Right, "paused", KeyInk) + 2;
    X = Say(Out, X, Row, Right, std::format("time x{:g}", Said.Speed), BarInk) + 2;
    X = Say(Out, X, Row, Right, std::format("mass {}", Mass(Said.Mass)), BarInk) + 2;
    Say(Out, X, Row, Right, std::format("power {:.2f}  angle {}", Said.Power, Said.Degrees), BarInk);
}

void PaintHelp(tui::Frame &Out, const Readout &Said)
{
    const std::pair<std::string, std::string> Keys[] = {
        {"left, right", "turn the aim"},
        {"up, down", "throw harder, softer"},
        {"shift with them", "in fine steps"},
        {"w a s d", "move the launcher"},
        {"enter, space", "throw a body"},
        {"[ ]", "a lighter body, a heavier one"},
        {"p", "pause, or go on"},
        {"+ -", "time faster, slower"},
        {"page up, page down", "nearer, farther"},
        {"1 2 3 4 5", "solar, binary, eight, chaos, lone"},
        {Said.Spelled[KeyClear], "clear the trails"},
        {Said.Spelled[KeyReset], "begin this sky again"},
        {Said.Spelled[KeyAim], "show the aim, or hide it"},
        {Said.Spelled[KeyHelp], "these keys"},
        {"esc", "leave"},
    };
    constexpr std::int32_t KeyWidth = 20;
    const painter::core::Size Size = Out.Size();
    const Rect Area = tui::Centred(Size, std::min(Size.Width - 2, 66), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const Rect Inside = tui::DrawWindow(Out, Area, "the sky's keys", "settings orbits keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        tui::PutText(Out, {Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        tui::PutText(Out, {Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk,
                     tui::WindowPaper);
        Row++;
    }
}

}  // namespace orbits
