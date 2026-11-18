/*
 * items_stand_keys.cpp — the keyboard's way to the things a program shows,
 * held on the host: the display's Input part (input.cpp) given keys as the
 * keyboard gives them — Tab from one thing to the next in the order made,
 * the arrows to the nearest, Enter pressing, a field typed into and its
 * caret moved, Escape leaving it, the labels' chord and a label typed; the
 * thing not to be used never reached; the ring drawn round what is reached;
 * a button's and a field's plate measured as the display lays it; the theme
 * insisting over what a program says.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "input.h"
#include "items_stand.h"
#include "painter/gui/measure.h"

/* The area the keyboard is in, as the stand gives it: the bench a check
 * holds, its events kept. */
namespace stand {
items::AreaState *KeyedArea = nullptr;
struct Told {
    std::uint16_t Kind;
    std::uint32_t Item;
    std::string   Bytes;
};
std::vector<Told> KeyedTold;
}  // namespace stand

std::optional<InputArea> AreasKeyed() noexcept
{
    if (!stand::KeyedArea || !InputReaches(*stand::KeyedArea)) return std::nullopt;
    return InputArea{stand::KeyedArea, [](std::uint16_t Kind, std::uint32_t Item, const void *Payload, std::uint32_t Bytes) {
                         stand::KeyedTold.push_back(
                             stand::Told{Kind, Item, std::string(static_cast<const char *>(Payload ? Payload : ""), Payload ? Bytes : 0)});
                     }};
}

namespace stand {

namespace {

using namespace std::chrono_literals;
using painter::core::Color;
using painter::core::Rect;
using painter::gui::Changes;
namespace gui = painter::gui;

kb_event_t Typed(char32_t Character, std::uint8_t Mods = 0) { return kb_event_t{static_cast<std::uint32_t>(Character), 0, Mods}; }
kb_event_t Key(std::uint8_t Scancode, std::uint8_t Mods = 0) { return kb_event_t{0, Scancode, static_cast<std::uint8_t>(Mods | KB_MOD_EXTENDED)}; }

std::string TextOf(const items::AreaState &Area, std::uint32_t Id)
{
    const items::Item *Of = Area.Items.Find(Id);
    return Of && Of->Said.Text ? *Of->Said.Text : std::string();
}

/* What the area was told since the last look: kind and item, a field's text. */
std::string Heard()
{
    std::string Made;
    for (const Told &Each : KeyedTold) {
        Made += Each.Kind == ITEM_EVENT_FOCUS ? (Each.Bytes.size() >= 1 && Each.Bytes[0] ? "came " : "left ")
              : Each.Kind == ITEM_EVENT_PRESS ? "press "
              : Each.Kind == ITEM_EVENT_TEXT  ? "text "
                                              : "? ";
        Made += std::to_string(Each.Item);
        if (Each.Kind == ITEM_EVENT_TEXT) Made += "=" + Each.Bytes.substr(sizeof(ItemBytes));
        Made += "; ";
    }
    KeyedTold.clear();
    return Made;
}

}  // namespace

void CheckKeys(Tally &Count)
{
    const items::Theme  Look = items::ThemeOf(0xD8D8E0, 0x181A28);
    items::ProgramState Program = HostProgram(Look, 1.0f);
    Bench               Shown(Program);
    Shown.Area.Place = Rect{0, 0, 480, 240};
    Shown.Area.Shown = true;
    Changes Say;
    /* A field; One and Two side by side; Three not to be used; a plain text
     * the keyboard does not reach. */
    Say.Create(1, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Field);
    Say.Place(1, {20, 20});
    Say.Size(1, {240, gui::ByContent});
    Say.Text(1, "");
    for (std::uint32_t Id = 2; Id <= 4; Id++) {
        Say.Create(Id, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Button);
        Say.Place(Id, {20 + 130 * static_cast<float>(Id - 2), 90});
        Say.Text(Id, Id == 2 ? "One" : Id == 3 ? "Two" : "Three");
    }
    Say.Disabled(4, true);
    Say.Create(5, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Body);
    Say.Text(5, "words");
    (void)Shown.Step(Say);
    items::Compositor       Composer(Rect{0, 0, 480, 240});
    items::AreaState *const Areas[] = {&Shown.Area};
    Composer.Take(Areas, Look);

    KeyedArea = &Shown.Area;
    KeyedTold.clear();
    InputStart(Rect{0, 0, 480, 240}, 1.0f, false, Look);
    KeyChord Point{};
    (void)KeyChordRead(reinterpret_cast<const std::uint8_t *>("ctrl+."), 6, &Point);
    InputPoint(Point);

    /* Tab reaches the field first; typing says its text; Tab, → and Enter. */
    (void)InputKey(Typed(KEY_TAB));
    const std::string Reached = Heard();
    (void)InputKey(Typed('h'));
    (void)InputKey(Typed('i'));
    const std::string Typing = Heard();
    const bool        Tab    = InputKey(Typed(KEY_TAB)) && Heard() == "left 1; came 2; ";
    const bool        Right  = InputKey(Key(KEY_RIGHT)) && Heard() == "left 2; came 3; ";
    const bool        Pushed = InputKey(Typed(KEY_ENTER)) && Heard() == "press 3; ";
    const bool        Wraps  = InputKey(Typed(KEY_TAB)) && Heard() == "left 3; came 1; ";
    const bool        Back   = InputKey(Typed(KEY_TAB, KB_MOD_SHIFT)) && Heard() == "left 1; came 3; ";
    Count.Check(Reached == "came 1; " && Typing == "text 1=h; text 1=hi; " && TextOf(Shown.Area, 1) == "hi" && Tab && Right && Pushed &&
                    Wraps && Back,
                "keys: Tab to the field, h i typed into it and told, Tab and → to the buttons, Enter pressing, Tab past the one not "
                "to be used and round, Shift+Tab back");

    /* A field's caret: Home, a character before all, End, Backspace; Escape
     * leaves it; a key that is not the display's goes on. */
    (void)InputKey(Typed(KEY_TAB));
    (void)Heard();
    (void)InputKey(Key(KEY_HOME));
    (void)InputKey(Typed('X'));
    (void)InputKey(Key(KEY_END));
    (void)InputKey(Typed(KEY_BACKSPACE));
    (void)InputKey(Key(KEY_DELETE));
    const std::string Edited = TextOf(Shown.Area, 1);
    const bool        Left   = InputKey(Typed(KEY_ESCAPE)) && Heard().ends_with("left 1; ") && Shown.Area.Focused == ITEM_AREA;
    const bool        On     = !InputKey(Typed('q')) && !InputKey(Key(KEY_RIGHT));
    Count.Check(Edited == "Xh" && Left && On,
                "field: Home, X before all, End, Backspace — \"" + Edited +
                    "\"; Escape leaves it; with nothing reached, a letter and an arrow go on to the program");

    /* The labels: one for each thing reached, the one typed reached and
     * pressed; nothing labelled not to be used. */
    const bool                    Shows = InputKey(Typed('.', KB_MOD_CTRL));
    const std::vector<InputLabel> Labels = InputLabels();
    std::string                   Words;
    for (const InputLabel &Each : Labels) Words += Each.Word + std::to_string(Each.Item) + " ";
    const bool Picked = InputKey(Typed('s')) && Heard() == "came 2; press 2; " && InputLabels().empty();
    Count.Check(Shows && Words == "a1 s2 d3 " && Picked,
                "labels: the chord shows a, s, d on the field and the two buttons — none on the one not to be used — and s reaches "
                "and presses One");

    /* The ring drawn round what is reached, a little out from it. */
    InputFrame(1000);
    const items::AreaState *Own  = InputOverlay();
    const items::Item      *Ring = Own ? Own->Items.Find(1) : nullptr;
    const items::Item      *One  = Shown.Area.Items.Find(2);
    const bool              Round = Ring && Ring->Said.Place && One && std::abs(Ring->Said.Place->X - (One->Box.X - 3) * ITEM_POINT) <= ITEM_POINT &&
                       std::abs(Ring->Said.Place->Y - (One->Box.Y - 3) * ITEM_POINT) <= ITEM_POINT;
    Count.Check(Round, "ring: drawn round what the keyboard reaches, three points out from it");

    /* A plate measured beforehand as the display lays it. */
    const items::Item *Field = Shown.Area.Items.Find(1), *Button = Shown.Area.Items.Find(2);
    const painter::core::PointF Measured =
        painter::gui::detail::MeasureOn(painter::gui::Glass::Pixels, "One", gui::Role::Button, gui::ByContent, {});
    Count.Check(Button && Button->Drawn && std::abs(Measured.X * ITEM_POINT - static_cast<float>(Button->Drawn->Measured.X)) <= 1 &&
                    std::abs(Measured.Y * ITEM_POINT - static_cast<float>(Button->Drawn->Measured.Y)) <= 1 && Field && Field->Drawn &&
                    Field->Drawn->Inset.X == 10 * ITEM_POINT,
                "plates: a button measured beforehand the size the display lays it, its plate's room round its words");

    /* The theme insisting: a button's own colour given way to the accent. */
    items::Properties Red;
    items::Fill       Own2;
    Own2.Color = ItemColor{65535, 0, 0, 65535};
    Red.Fills  = std::vector<items::Fill>{Own2};
    items::Theme Insisting = Look;
    Insisting.Insist       = true;
    const std::vector<items::Fill> Said   = items::FillsOf(Red, ITEM_SHAPE, ITEM_ROLE_BUTTON, Look);
    const std::vector<items::Fill> Theirs = items::FillsOf(Red, ITEM_SHAPE, ITEM_ROLE_BUTTON, Insisting);
    const std::vector<items::Fill> Plain  = items::FillsOf(Red, ITEM_SHAPE, ITEM_ROLE_NONE, Insisting);
    Count.Check(Said.size() == 1 && Said[0].Kind == ITEM_FILL_COLOR && Theirs.size() == 1 && Theirs[0].Kind == ITEM_FILL_THEME &&
                    Theirs[0].Theme == ITEM_THEME_ACCENT && Plain.size() == 1 && Plain[0].Kind == ITEM_FILL_COLOR,
                "insist: a button's red its own until the person insists — then the theme's accent; a drawing with no role keeps "
                "its colours");
    KeyedArea = nullptr;
}

}  // namespace stand
