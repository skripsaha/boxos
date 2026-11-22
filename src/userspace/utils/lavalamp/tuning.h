/* tuning.h — the lamp's keys and the panel that tunes the lamp on the screen
 * while it flows: a row for each thing a composition is made of, and for
 * the way it is drawn and whether its changes are remembered, named as the
 * settings name them. What the keys do is here, once; each face only draws
 * it.
 *
 *   enter, space, tab     the panel, opened
 *   left, right           the palette before or after
 *   up, down              the flow faster or slower
 *   esc, q                leave
 *
 * In the panel: up and down (tab) choose a row, left and right (- +) change
 * it — a colour turns round the wheel, a switch goes off or on — enter types
 * a colour or turns a switch over, esc closes it. */
#ifndef LAVALAMP_TUNING_H
#define LAVALAMP_TUNING_H

#include <cstdint>
#include <string>
#include <string_view>

#include "painter/tui/key.h"

#include "composition.h"
#include "look.h"

namespace lavalamp {

enum class Row : std::uint8_t { Palette, Wax, Rim, Glass, Blobs, Size, Flow, Glow, Light, Tui, Remember, Count };

inline constexpr int Rows = static_cast<int>(Row::Count);

/* What a key did. */
enum class Heard : std::uint8_t {
    Nothing,
    Panel,     // only the panel changed: opened, closed, a row chosen, a letter typed
    Lamp,      // the composition changed
    Face,      // tui turned on or off: the lamp drawn the other way
    Remember,  // remember turned on or off
    Leave,
};

class Tuning {
public:
    Heard Key(const painter::tui::KeyEvent &Key, Look &Now);
    /* A note in the panel, until the next key. */
    void  Say(std::string Note) { Said = std::move(Note); }

    bool             Open() const noexcept { return Shown; }
    Row              Chosen() const noexcept { return At; }
    bool             Typing() const noexcept { return Editing; }
    std::string_view Typed() const noexcept { return Text; }
    /* What the chosen row has to say of the last key, or nothing. */
    std::string_view Note() const noexcept { return Said; }
    /* The line of keys for what the panel is doing now. */
    std::string_view Hint() const noexcept;

    static std::string_view Name(Row Of);
    static bool             IsColor(Row Of) noexcept { return Of == Row::Wax || Of == Row::Rim || Of == Row::Glass; }
    static bool             IsSwitch(Row Of) noexcept { return Of == Row::Light || Of == Row::Tui || Of == Row::Remember; }
    static bool             IsNumber(Row Of) noexcept { return Of != Row::Palette && !IsColor(Of) && !IsSwitch(Of); }
    /* The row's value as it is shown; for a number, how far it is between
     * its least and its most, 0 to 1; for a switch, whether it is on. */
    static std::string Value(Row Of, const Look &Now);
    static float       Share(Row Of, const Look &Now);
    static bool        On(Row Of, const Look &Now);
    static Color       ColorOf(Row Of, const Look &Now);

private:
    Heard OnLamp(const painter::tui::KeyEvent &Key, Composition &Lamp);
    Heard OnPanel(const painter::tui::KeyEvent &Key, Look &Now);
    Heard OnTyping(const painter::tui::KeyEvent &Key, Composition &Lamp);
    Heard Change(int By, Look &Now);
    /* A switch set: By below 0 off, above 0 on, 0 the other way. */
    Heard Turn(int By, Look &Now);
    void  Choose(int By);

    bool        Shown   = false;
    bool        Editing = false;
    Row         At      = Row::Palette;
    std::string Text;
    std::string Said;
};

}  // namespace lavalamp

#endif
