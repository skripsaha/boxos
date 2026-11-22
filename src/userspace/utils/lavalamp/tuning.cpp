/* tuning.cpp — the lamp's keys and its panel's rows. See tuning.h. */

#include "tuning.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace lavalamp {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;

constexpr float HueStep = 0.2618f;   // a colour's turn round the wheel for one key: 15 degrees

constexpr std::string_view Names[Rows] = {"palette", "wax", "rim", "glass", "blobs", "size", "flow", "glow", "light", "tui", "remember"};

struct Range {
    int Least, Most, Step;
};

Range RangeOf(Row Of)
{
    switch (Of) {
    case Row::Blobs: return {limits::LeastBlobs, limits::MostBlobs, 1};
    case Row::Size: return {limits::LeastSize, limits::MostSize, 10};
    case Row::Flow: return {limits::LeastFlow, limits::MostFlow, 1};
    default: return {limits::LeastGlow, limits::MostGlow, 10};
    }
}

int &NumberOf(Row Of, Composition &Lamp)
{
    switch (Of) {
    case Row::Blobs: return Lamp.Blobs;
    case Row::Size: return Lamp.Size;
    case Row::Flow: return Lamp.Flow;
    default: return Lamp.Glow;
    }
}

Color &PaintOf(Row Of, Composition &Lamp)
{
    return Of == Row::Wax ? Lamp.Wax : Of == Row::Rim ? Lamp.Rim : Lamp.Glass;
}

bool &SwitchOf(Row Of, Look &Now)
{
    return Of == Row::Light ? Now.Lamp.Light : Of == Row::Tui ? Now.Cells : Now.Remember;
}

/* A colour turned round the wheel as the eye sees it — its lightness and how
 * colourful it is kept — By turns of HueStep. A grey has no way round. */
Color Turned(Color From, int By)
{
    const painter::core::Oklab Was =
        painter::core::OklabOf(static_cast<float>(From.Red()) / 255, static_cast<float>(From.Green()) / 255, static_cast<float>(From.Blue()) / 255);
    const float Angle = HueStep * static_cast<float>(By);
    const float A = Was.A * std::cos(Angle) - Was.B * std::sin(Angle), B = Was.A * std::sin(Angle) + Was.B * std::cos(Angle);
    float       R = 0, G = 0, Bl = 0;
    painter::core::RgbOf(painter::core::Oklab{Was.L, A, B}, R, G, Bl);
    const auto Byte = [](float Of) { return static_cast<std::uint8_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 255)); };
    return Color::Rgb(Byte(R), Byte(G), Byte(Bl));
}

bool IsHex(char32_t C)
{
    return (C >= U'0' && C <= U'9') || (C >= U'a' && C <= U'f') || (C >= U'A' && C <= U'F');
}

bool Leaves(const painter::tui::KeyEvent &Key)
{
    return Key.IsCtrl() && (Key.Character == U'q' || Key.Character == U'Q');
}

}  // namespace

std::string_view Tuning::Name(Row Of)
{
    return Names[static_cast<int>(Of)];
}

std::string Tuning::Value(Row Of, const Look &Now)
{
    const Composition &Lamp = Now.Lamp;
    if (IsSwitch(Of)) return On(Of, Now) ? "on" : "off";
    switch (Of) {
    case Row::Palette: return Lamp.PaletteName();
    case Row::Wax: return Spelled(Lamp.Wax);
    case Row::Rim: return Spelled(Lamp.Rim);
    case Row::Glass: return Spelled(Lamp.Glass);
    case Row::Size: return std::format("{}%", Lamp.Size);
    case Row::Glow: return std::format("{}%", Lamp.Glow);
    case Row::Blobs: return std::format("{}", Lamp.Blobs);
    default: return std::format("{}", Lamp.Flow);
    }
}

float Tuning::Share(Row Of, const Look &Now)
{
    if (!IsNumber(Of)) return 0;
    const Range Within = RangeOf(Of);
    Composition Read   = Now.Lamp;
    return static_cast<float>(NumberOf(Of, Read) - Within.Least) / static_cast<float>(Within.Most - Within.Least);
}

bool Tuning::On(Row Of, const Look &Now)
{
    if (!IsSwitch(Of)) return false;
    Look Read = Now;
    return SwitchOf(Of, Read);
}

Color Tuning::ColorOf(Row Of, const Look &Now)
{
    Composition Read = Now.Lamp;
    return PaintOf(Of, Read);
}

std::string_view Tuning::Hint() const noexcept
{
    if (Editing) return "#rrggbb, enter takes it    esc leaves it as it was";
    if (Shown && IsSwitch(At)) return "↑ ↓ choose    ← → off, on    enter turns it over    esc closes";
    if (Shown) return "↑ ↓ choose    ← → change    enter types a colour    esc closes";
    return "enter tunes the lamp    ← → palette    ↑ ↓ flow    esc leaves";
}

Heard Tuning::Key(const painter::tui::KeyEvent &Key, Look &Now)
{
    if (Leaves(Key)) return Heard::Leave;
    if (Editing) return OnTyping(Key, Now.Lamp);
    if (Shown) return OnPanel(Key, Now);
    return OnLamp(Key, Now.Lamp);
}

/* The panel closed: the palettes and the flow are at hand all the same. */
Heard Tuning::OnLamp(const painter::tui::KeyEvent &Key, Composition &Lamp)
{
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case KeyCodes::Left:
        case KeyCodes::Right: {
            const int Count = static_cast<int>(Palettes.size());
            const int Step  = Key.Scancode == KeyCodes::Left ? Count - 1 : 1;
            Lamp.Take(Lamp.Palette == Own ? 0 : (Lamp.Palette + Step) % Count);
            return Heard::Lamp;
        }
        case KeyCodes::Up:
        case KeyCodes::Down: {
            const int Was = Lamp.Flow;
            Lamp.Flow     = std::clamp(Lamp.Flow + (Key.Scancode == KeyCodes::Up ? 1 : -1), limits::LeastFlow, limits::MostFlow);
            return Lamp.Flow == Was ? Heard::Nothing : Heard::Lamp;
        }
        default: return Heard::Nothing;
        }
    }
    switch (Key.Character) {
    case KeyCodes::Escape:
    case U'q':
    case U'Q': return Heard::Leave;
    case KeyCodes::Enter:
    case KeyCodes::Tab:
    case U' ':
        Shown = true;
        Said.clear();
        return Heard::Panel;
    default: return Heard::Nothing;
    }
}

Heard Tuning::OnPanel(const painter::tui::KeyEvent &Key, Look &Now)
{
    /* A note answers the last key, so each key clears it first. */
    Said.clear();
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case KeyCodes::Up: Choose(-1); return Heard::Panel;
        case KeyCodes::Down: Choose(1); return Heard::Panel;
        case KeyCodes::Left: return Change(-1, Now);
        case KeyCodes::Right: return Change(1, Now);
        default: return Heard::Panel;
        }
    }
    switch (Key.Character) {
    case KeyCodes::Escape: Shown = false; return Heard::Panel;
    case U'q':
    case U'Q': return Heard::Leave;
    case KeyCodes::Tab: Choose(Key.IsShift() ? -1 : 1); return Heard::Panel;
    case U'-': return Change(-1, Now);
    case U'+':
    case U'=': return Change(1, Now);
    case U' ':
        if (IsSwitch(At)) return Turn(0, Now);
        return Heard::Panel;
    case KeyCodes::Enter:
        if (IsSwitch(At)) return Turn(0, Now);
        if (IsColor(At)) {
            Editing = true;
            Text    = "#";
        } else Said = "← → change it";
        return Heard::Panel;
    default: return Heard::Panel;
    }
}

Heard Tuning::OnTyping(const painter::tui::KeyEvent &Key, Composition &Lamp)
{
    if (Key.IsExtended()) return Heard::Nothing;
    Said.clear();
    const char32_t C = Key.Character;
    if (C == KeyCodes::Escape) {
        Editing = false;
        return Heard::Panel;
    }
    if (C == KeyCodes::Enter) {
        const std::optional<Color> Got = ColorTyped(Text);
        if (!Got) {
            /* What was typed stays: a colour a digit short is a digit from right. */
            Said = "not a colour";
            return Heard::Panel;
        }
        PaintOf(At, Lamp) = *Got;
        Lamp.Palette      = Own;
        Editing           = false;
        return Heard::Lamp;
    }
    if (C == KeyCodes::Backspace || C == 0x7F) {
        if (!Text.empty()) Text.pop_back();
        return Heard::Panel;
    }
    if ((C == U'#' && Text.empty()) || (IsHex(C) && Text.size() < 7)) Text += static_cast<char>(C);
    return Heard::Panel;
}

void Tuning::Choose(int By)
{
    At = static_cast<Row>((static_cast<int>(At) + By + Rows) % Rows);
}

Heard Tuning::Change(int By, Look &Now)
{
    if (IsSwitch(At)) return Turn(By, Now);
    Composition &Lamp = Now.Lamp;
    if (At == Row::Palette) {
        const int Count = static_cast<int>(Palettes.size());
        Lamp.Take(Lamp.Palette == Own ? (By > 0 ? 0 : Count - 1) : (Lamp.Palette + By + Count) % Count);
        return Heard::Lamp;
    }
    if (IsColor(At)) {
        Color &Paint = PaintOf(At, Lamp);
        const Color Was = Paint;
        Paint           = Turned(Paint, By);
        if (Paint == Was) {
            Said = "a grey has no hue: enter types one";
            return Heard::Panel;
        }
        Lamp.Palette = Own;
        return Heard::Lamp;
    }
    const Range Within = RangeOf(At);
    int        &Number = NumberOf(At, Lamp);
    const int   Was    = Number;
    Number             = std::clamp(Number + By * Within.Step, Within.Least, Within.Most);
    return Number == Was ? Heard::Panel : Heard::Lamp;
}

Heard Tuning::Turn(int By, Look &Now)
{
    bool      &Switch = SwitchOf(At, Now);
    const bool Was    = Switch;
    Switch            = By < 0 ? false : By > 0 ? true : !Switch;
    if (Switch == Was) return Heard::Panel;
    switch (At) {
    case Row::Tui: return Heard::Face;
    case Row::Remember: return Heard::Remember;
    default: return Heard::Lamp;
    }
}

}  // namespace lavalamp
