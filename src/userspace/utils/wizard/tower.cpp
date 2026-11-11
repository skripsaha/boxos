/* tower.cpp — the wizard's tower: the cauldron, the casting and the keys. */

#include "tower.h"

#include <algorithm>
#include <format>
#include <utility>

#include "box/clock.h"
#include "box/cxx/glass.h"

namespace wizard {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::uint32_t FrameMs = 70;          // the picture moves some fourteen times a second
constexpr std::uint64_t StatusMs = 4500;       // how long a word of the tower's own stays up

}  // namespace

Tower::Tower(tui::Stage Opened, std::vector<std::string> Asked)
    : Screen(std::move(Opened)), Shown(ReadLook()), Luck(static_cast<std::uint32_t>(clock_uptime_us())),
      Words(std::move(Asked))
{
}

std::optional<std::string> Tower::Misunderstood(const std::vector<std::string> &Words)
{
    for (const std::string &One : Words) {
        if (CharmNamed(One) != nullptr) {
            if (Words.size() > 1) return "say one charm, or things for the cauldron, and not both";
        } else if (IngredientNamed(One) == NoIngredient) {
            return std::format("{} is neither a charm nor a thing for the cauldron", One);
        }
    }
    return std::nullopt;
}

void Tower::Run()
{
    Screen.HearEnd();
    LastMs = clock_uptime_ms();
    Door();
    for (;;) {
        if (Watch.changed()) Shown = ReadLook();
        const std::uint64_t Now = clock_uptime_ms();
        Step(static_cast<std::uint32_t>(Now - LastMs));
        LastMs = Now;
        Paint();
        if (!Screen.Present()) break;

        const tui::Event Heard = Screen.Wait(FrameMs);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) break;
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key)) break;
    }
    if (LaidUntilMs != 0) Lift();
}

// What was asked at the door: a charm cast at once, or the things put in and
// drunk.
void Tower::Door()
{
    if (Words.empty()) return;
    if (const Charm *Asked = CharmNamed(Words[0]); Asked != nullptr) {
        Lay(Spell(*Asked, Luck, Shown));
        return;
    }
    for (const std::string &One : Words) Put(IngredientNamed(One));
    Drink();
}

std::string Tower::Spelled(WizardKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

void Tower::Remark(std::string Words)
{
    Status = std::move(Words);
    StatusUntilMs = clock_uptime_ms() + StatusMs;
}

std::uint32_t Tower::LeftSeconds() const
{
    const std::uint64_t Now = clock_uptime_ms();
    return LaidUntilMs > Now ? static_cast<std::uint32_t>((LaidUntilMs - Now + 999) / 1000) : 0;
}

bool Tower::OnKey(const tui::KeyEvent &Key)
{
    if (Help) {
        Help = false;
        return true;
    }
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](WizardKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) Help = true;
    else if (Is(KeySpell)) CastSpell();
    else if (Is(KeyDrink)) Drink();
    else if (Is(KeyEmpty)) Empty();
    else if (Is(KeyLift)) Lift();
    else if (Key.IsCtrl() || Key.IsAlt() || Key.IsExtended()) return true;
    else if (Key.Ascii == tui::KeyCodes::Escape) return false;
    else if (Key.Ascii == tui::KeyCodes::Backspace && !Mixed.empty()) {
        Remark(std::format("{} comes out again", Ingredients()[Mixed.back()].Name));
        Mixed.pop_back();
    } else if (Key.Ascii >= '1' && Key.Ascii < '1' + static_cast<char>(IngredientCount)) {
        Put(static_cast<std::size_t>(Key.Ascii - '1'));
    }
    return true;
}

void Tower::CastSpell()
{
    Lay(Spell(Luck, Shown, LastCharm));
}

void Tower::Put(std::size_t Ingredient)
{
    if (Mixed.size() >= CauldronMost) {
        Remark(std::format("The cauldron is full: drink it with {}, or pour it out with {}", Spelled(KeyDrink),
                        Spelled(KeyEmpty)));
        return;
    }
    Mixed.push_back(static_cast<std::uint8_t>(Ingredient));
    const Scene Where = Placed();
    const core::Point Surface = SurfaceOf(Where.CauldronX, Where.CauldronY);
    Burst(Surface.X, Surface.Y, Color::FromRaw(Liquid(Mixed)), 6);
    Remark(std::format("{} goes in", Ingredients()[Ingredient].Name));
}

void Tower::Drink()
{
    if (Mixed.empty()) {
        Remark("The cauldron is empty: put things in it with 1 to 8");
        return;
    }
    Casting Brewed = Potion(Mixed, Luck, Shown);
    Mixed.clear();
    Lay(std::move(Brewed));
}

void Tower::Empty()
{
    if (Mixed.empty()) return;
    Mixed.clear();
    Remark("Poured out");
}

void Tower::Lift()
{
    const box::result<bool> Lifted = box::glass::lift();
    LaidUntilMs = 0;
    if (!Lifted) Remark(std::format("The glass would not let go: {}", Lifted.error().message()));
    else if (*Lifted) Remark("The spell is lifted");
}

// What is to be laid over the glass, laid — or why it is not.
void Tower::Lay(Casting Spelt)
{
    LastCharm = Spelt.Of != nullptr ? Spelt.Of : LastCharm;
    if (Spelt.Kinds == 0) {
        Spoken = std::move(Spelt);
        Remark("Nothing comes of it: settings wizard effects says which may be cast");
        return;
    }
    const box::status Laid = box::glass::lay({.kinds = Spelt.Kinds,
                                              .strength = Spelt.Strength,
                                              .axes = Spelt.Axes,
                                              .color = Spelt.Color,
                                              .duration = std::chrono::milliseconds(Spelt.DurationMs)});
    if (!Laid) {
        Remark(std::format("The glass would not take it: {}", Laid.error().message()));
        return;
    }
    LaidUntilMs = clock_uptime_ms() + Spelt.DurationMs;
    Excitement = 255;
    const Scene Where = Placed();
    const core::Point Stone = StoneOf(Where.WizardX, Where.WizardY);
    Burst(Stone.X, Stone.Y, Shown.Glow, 28);
    Spoken = std::move(Spelt);
}

// A handful of specks leaving X, Y in every direction.
void Tower::Burst(std::int32_t X, std::int32_t Y, Color Ink, std::uint32_t Count)
{
    for (std::uint32_t One = 0; One < Count; One++) {
        const auto Around = static_cast<std::int32_t>(Luck.Below(61)) - 30;
        const auto Up = static_cast<std::int32_t>(Luck.Below(50)) + 10;
        Sparks.Add(Speck{X * 16, Y * 16, -Up * 16, Around * 16, 700u + Luck.Below(700), 0, Ink, true});
    }
}

// What moves of itself: the specks, the bubbles, the stone's excitement, and
// the spell wearing off.
void Tower::Step(std::uint32_t ElapsedMs)
{
    const std::uint64_t Now = LastMs;
    if (LaidUntilMs != 0 && Now >= LaidUntilMs) {
        LaidUntilMs = 0;
        Remark("The spell wears off");
    }
    Excitement -= std::min<std::uint32_t>(Excitement, ElapsedMs * 255u / 1500u);
    Sparks.Step(ElapsedMs);

    const Scene Where = Placed();
    const core::Point Surface = SurfaceOf(Where.CauldronX, Where.CauldronY);
    const core::Point Stone = StoneOf(Where.WizardX, Where.WizardY);
    BubbleMs += ElapsedMs;
    const std::uint32_t Every = 700u / (static_cast<std::uint32_t>(Mixed.size()) * 2u + 1u) + 60u;
    while (BubbleMs >= Every) {
        BubbleMs -= Every;
        const auto Across = static_cast<std::int32_t>(Luck.Below(9)) - 4;
        const Color Ink = Mixed.empty() ? Color::Rgb(0x70, 0x90, 0x78) : Color::Mix(Color::FromRaw(Liquid(Mixed)), Color::Rgb(255, 255, 255), 80);
        Sparks.Add(Speck{(Surface.X + Across) * 16, Surface.Y * 16, -static_cast<std::int32_t>(10 + Luck.Below(14)) * 16, 0,
                         900u + Luck.Below(700), 0, Ink, true});
        if (Mixed.size() >= 2)
            Sparks.Add(Speck{(Surface.X + Across) * 16, (Surface.Y - 2) * 16, -static_cast<std::int32_t>(5 + Luck.Below(5)) * 16,
                             (static_cast<std::int32_t>(Luck.Below(7)) - 3) * 16, 1800, 0, Color::Rgb(0xD0, 0xD0, 0xD8), false});
        if (Luck.Below(4) == 0)
            Sparks.Add(Speck{Stone.X * 16, Stone.Y * 16, -static_cast<std::int32_t>(4 + Luck.Below(10)) * 16,
                             (static_cast<std::int32_t>(Luck.Below(21)) - 10) * 16, 1000, 0, Shown.Glow, true});
    }
}

}  // namespace wizard
