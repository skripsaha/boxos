/* tuning.h — the lamp's keys and the panel that tunes the lamp on the screen
 * while it flows: a row for each thing a composition is made of, named as
 * the settings name it. What the keys do is here, once; each face only
 * draws it.
 *
 *   enter, space, tab     the panel, opened
 *   left, right           the palette before or after
 *   up, down              the flow faster or slower
 *   esc, q                leave
 *
 * In the panel: up and down (tab) choose a row, left and right (- +) change
 * it — a colour turns round the wheel — enter types a colour, esc closes it. */
#ifndef LAVALAMP_TUNING_H
#define LAVALAMP_TUNING_H

#include <cstdint>
#include <string>
#include <string_view>

#include "painter/tui/key.h"

#include "composition.h"

namespace lavalamp {

enum class Row : std::uint8_t { Palette, Wax, Rim, Glass, Blobs, Size, Flow, Glow, Count };

inline constexpr int Rows = static_cast<int>(Row::Count);

/* What a key did. */
enum class Heard : std::uint8_t {
    Nothing,
    Panel,     // only the panel changed: opened, closed, a row chosen, a letter typed
    Lamp,      // the composition changed
    Leave,
};

class Tuning {
public:
    Heard Key(const painter::tui::KeyEvent &Key, Composition &Lamp);

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
    /* The row's value as it is shown, and, for a number, how far it is
     * between its least and its most, 0 to 1. */
    static std::string Value(Row Of, const Composition &Lamp);
    static float       Share(Row Of, const Composition &Lamp);
    static Color       ColorOf(Row Of, const Composition &Lamp);

private:
    Heard OnLamp(const painter::tui::KeyEvent &Key, Composition &Lamp);
    Heard OnPanel(const painter::tui::KeyEvent &Key, Composition &Lamp);
    Heard OnTyping(const painter::tui::KeyEvent &Key, Composition &Lamp);
    Heard Change(int By, Composition &Lamp);
    void  Choose(int By);

    bool        Shown   = false;
    bool        Editing = false;
    Row         At      = Row::Palette;
    std::string Text;
    std::string Said;
};

}  // namespace lavalamp

#endif
