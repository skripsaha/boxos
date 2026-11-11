/* tower.cpp — the wizard's tower: the loop, the keys and what they do. */

#include "tower.h"

#include <algorithm>
#include <format>
#include <utility>

#include "box/clock.h"
#include "box/cxx/glass.h"

#include "cauldron.h"

namespace wizard {

namespace {

namespace tui = painter::tui;

constexpr std::uint32_t FrameMs = 70;             // the picture moves some fourteen times a second
constexpr std::uint64_t DrinkLayMs = 760;         // the vial is at his lips, and the potion takes hold
constexpr std::uint64_t CastLayMs = 480;          // the staff is up, and the spell takes hold

}  // namespace

Tower::Tower(tui::Stage Opened, std::vector<std::string> Asked)
    : Screen(std::move(Opened)), Chosen(ReadLook()), Luck(static_cast<std::uint32_t>(clock_uptime_us())), Words(std::move(Asked)),
      Where(Arranged(Screen.Size().Width, Screen.Size().Height)), Picture(Where.Width, Where.Height)
{
    Hall.Build(Where, Chosen);
    Wizard.Build(Where, Chosen);
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
        if (Watch.changed()) {
            Chosen = ReadLook();
            Hall.Build(Where, Chosen);
            Wizard.Build(Where, Chosen);
        }
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
// drunk; and with nothing asked, a greeting.
void Tower::Door()
{
    if (Words.empty()) {
        Tell("The Old Wizard", std::format("Ah, a visitor! Choose a thing with the arrows, drop it in the pot with space and drink the brew with enter. "
                                           "Or ask for a spell with {}.", Spelled(KeySpell)));
        return;
    }
    if (const Charm *Asked = CharmNamed(Words[0]); Asked != nullptr) {
        Cast(*Asked);
        return;
    }
    for (const std::string &One : Words)
        if (Brewing.size() < CauldronMost) Brewing.push_back(static_cast<std::uint8_t>(IngredientNamed(One)));
    Drink();
}

std::string Tower::Spelled(WizardKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chosen.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

bool Tower::OnKey(const tui::KeyEvent &Key)
{
    if (Reading.Open) return OnBookKey(Key);
    if (Help) {
        Help = false;
        return true;
    }
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](WizardKey Which) { return KeyChordMatches(&Chosen.Keys[Which], &Raw); };
    if (Is(KeyHelp)) Help = true;
    else if (Is(KeySpell)) CastRandom();
    else if (Is(KeyDrink)) Drink();
    else if (Is(KeyEmpty)) Empty();
    else if (Is(KeyLift)) Lift();
    else if (Is(KeyBook)) Reading.Open = true;
    else if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:     Browse(-1); break;
        case tui::KeyCodes::Right:    Browse(1); break;
        case tui::KeyCodes::PageUp:   Browse(-4); break;
        case tui::KeyCodes::PageDown: Browse(4); break;
        case tui::KeyCodes::Home:     Browse(-static_cast<int>(IngredientCount)); break;
        case tui::KeyCodes::End:      Browse(static_cast<int>(IngredientCount)); break;
        default: break;
        }
    } else if (Key.IsCtrl() || Key.IsAlt()) {
        return true;
    } else if (Key.Ascii == tui::KeyCodes::Escape) {
        return false;
    } else if (Key.Ascii == tui::KeyCodes::Enter) {
        Drink();
    } else if (Key.Ascii == ' ') {
        Throw();
    } else if (Key.Ascii == tui::KeyCodes::Backspace) {
        TakeBack();
    }
    return true;
}

// The grimoire's keys: tab or the arrows turn the page, up and down choose, enter casts a spell chosen.
bool Tower::OnBookKey(const tui::KeyEvent &Key)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const std::size_t Count = Reading.Page == 0 ? RecipeCount : CharmCount;
    int &Cursor = Reading.Cursor[Reading.Page];
    if (KeyChordMatches(&Chosen.Keys[KeyBook], &Raw) || (!Key.IsExtended() && Key.Ascii == tui::KeyCodes::Escape)) {
        Reading.Open = false;
    } else if (!Key.IsExtended() && Key.Ascii == tui::KeyCodes::Tab) {
        Reading.Page = 1 - Reading.Page;
    } else if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:
        case tui::KeyCodes::Right:    Reading.Page = 1 - Reading.Page; break;
        case tui::KeyCodes::Up:       Cursor = (Cursor + static_cast<int>(Count) - 1) % static_cast<int>(Count); break;
        case tui::KeyCodes::Down:     Cursor = (Cursor + 1) % static_cast<int>(Count); break;
        case tui::KeyCodes::PageUp:   Cursor = std::max(0, Cursor - 6); break;
        case tui::KeyCodes::PageDown: Cursor = std::min(static_cast<int>(Count) - 1, Cursor + 6); break;
        default: break;
        }
    } else if (Key.Ascii == tui::KeyCodes::Enter && Reading.Page == 1) {
        Reading.Open = false;
        Cast(Charms()[static_cast<std::size_t>(Cursor)]);
    }
    return true;
}

void Tower::Browse(int By)
{
    const int Count = static_cast<int>(IngredientCount);
    int Next = static_cast<int>(Pick) + By;
    Next = std::abs(By) == 1 ? (Next + Count) % Count : std::clamp(Next, 0, Count - 1);
    Pick = static_cast<std::size_t>(Next);
    const Ingredient &Thing = Ingredients()[Pick];
    Tell(std::string(Thing.Name), std::string(Thing.Looks));
    GazeUntilMs = LastMs + 1500;
    GazeToward = 0.0f;
    GazeTowardUp = -0.8f;
}

// The chosen thing leaves his hand for the pot; it is in the brew when it lands.
void Tower::Throw()
{
    if (Falling) return;
    if (Brewing.size() >= CauldronMost) {
        Tell("The cauldron is full", std::format("Drink it with enter, take the last thing out with backspace, or pour it all out with {}.", Spelled(KeyEmpty)));
        return;
    }
    Falling = Flight{Pick, LastMs, WizardPixelX(Where, 36.0f), WizardPixelY(Where, 74.0f)};
    GazeUntilMs = LastMs + FlightMs + 500;
    GazeToward = 1.0f;
    GazeTowardUp = -0.3f;
}

void Tower::TakeBack()
{
    if (Brewing.empty()) {
        Tell("Nothing to take out", "The cauldron is empty. Choose a thing with the arrows and drop it in with space.");
        return;
    }
    const Ingredient &Thing = Ingredients()[Brewing.back()];
    const Spot Surface = SurfaceOf(Where);
    Burst(Surface.X, Surface.Y, Color::FromRaw(Thing.Color), 8, SpeckKind::Drop);
    Tell(std::string(Thing.Name), std::format("{} comes out again.", Thing.Name));
    Brewing.pop_back();
    Splash = 0.8f;
}

void Tower::Drink()
{
    if (Soon) return;
    if (Brewing.empty()) {
        Tell("The cauldron is empty", "Choose a thing with the arrows and drop it in with space; then enter drinks the brew.");
        return;
    }
    Soon = Pending{Potion(Brewing, Luck, Chosen), LastMs + DrinkLayMs, true};
    DrinkUntilMs = LastMs + DrinkLayMs + 700;
    GazeUntilMs = DrinkUntilMs;
    GazeToward = 0.0f;
    GazeTowardUp = -0.2f;
    Tell("Down the hatch", "Bottoms up!");
}

void Tower::CastRandom()
{
    Cast(ChooseCharm(Luck, Chosen, LastCharm));
}

void Tower::Cast(const Charm &Which)
{
    if (Soon) return;
    Soon = Pending{Spell(Which, Luck, Chosen), LastMs + CastLayMs, false};
    CastUntilMs = LastMs + CastLayMs + 1300;
    GazeUntilMs = CastUntilMs;
    GazeToward = 0.0f;
    GazeTowardUp = 0.9f;
    Excite = std::max(Excite, 0.35f);
}

void Tower::Empty()
{
    if (Brewing.empty()) return;
    const Spot Surface = SurfaceOf(Where);
    Burst(Surface.X, Surface.Y, Color::FromRaw(Liquid(Brewing)), 14, SpeckKind::Drop);
    Brewing.clear();
    Splash = 1.0f;
    Tell("Poured out", "Hiss! A waste of good toad.");
}

void Tower::Lift()
{
    const box::result<bool> Lifted = box::glass::lift();
    LaidUntilMs = 0;
    if (!Lifted) Tell("Stuck", std::format("The glass would not let go: {}", Lifted.error().message()));
    else if (*Lifted) Tell("Lifted", "The spell is lifted. The room is as it was.");
}

// What is to be laid over the glass, laid — or why it is not.
void Tower::Lay(Casting Spelt)
{
    LastCharm = Spelt.Of != nullptr ? Spelt.Of : LastCharm;
    if (Spelt.Kinds == 0) {
        Tell(Spelt.Name, "Nothing comes of it: settings wizard effects says which may be cast.");
        Spoken = std::move(Spelt);
        return;
    }
    const box::status Laid = box::glass::lay({.kinds = Spelt.Kinds,
                                              .strength = Spelt.Strength,
                                              .axes = Spelt.Axes,
                                              .color = Spelt.Color,
                                              .duration = std::chrono::milliseconds(Spelt.DurationMs)});
    if (!Laid) {
        Tell("The glass is deaf", std::format("The glass would not take it: {}", Laid.error().message()));
        return;
    }
    LaidUntilMs = LastMs + Spelt.DurationMs;
    Excite = 1.0f;
    Face = Spelt.Face;
    MoodWeight = 1.0f;
    Burst(WizardPixelX(Where, -38.0f), WizardPixelY(Where, 8.0f), Chosen.Glow, 34, SpeckKind::Spark);
    if (Spelt.Of != nullptr) Book.CountCast();
    else Book.CountDrunk();
    if (Spelt.Found != NoIngredient && Book.Learn(Spelt.Found))
        Tell(std::format("NEW: {}", Spelt.Name), std::format("{} It is written in the grimoire ({}).", Spelt.Verse, Spelled(KeyBook)));
    else
        Tell(Spelt.Name, Spelt.Verse);
    Spoken = std::move(Spelt);
}

}  // namespace wizard
