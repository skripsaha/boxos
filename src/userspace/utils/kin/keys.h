#ifndef KIN_KEYS_H
#define KIN_KEYS_H

/* Keys the way edit and read hear them and tell them: each program keeps every
 * key it answers in one table of its own, the keys are heard from that table
 * and the help is written from it, so the help can never say a key does what
 * it does not — and the two programs say the same key the same way.
 *
 * A key with Shift that is not bound with Shift is heard as the key without
 * it; on a key that moves, Shift makes the selection grow.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "painter/tui/stage.h"

#include "look.h"

namespace kin {

/* A key with its modifiers: a character, or one of the keys that are no
 * character (Key true, Code its scancode). AnyDigit takes Ctrl and any digit. */
struct Chord {
    bool         Ctrl     = false;
    bool         Shift    = false;
    bool         Key      = false;
    std::uint8_t Code     = 0;
    bool         AnyDigit = false;
};

constexpr Chord CtrlLetter(char Letter, bool Shift = false)
{
    return {true, Shift, false, static_cast<std::uint8_t>(Letter)};
}
constexpr Chord Plain(char Ch, bool Shift = false) { return {false, Shift, false, static_cast<std::uint8_t>(Ch)}; }
constexpr Chord KeyOnly(std::uint8_t Code, bool Ctrl = false, bool Shift = false) { return {Ctrl, Shift, true, Code}; }
constexpr Chord CtrlDigit() { return {true, false, false, 0, true}; }

/* A binding whose words are empty is said by the line before it: its key is
 * named there too, unless that line names its keys in words of its own. */
template <class Command, class Part>
struct Binding {
    Chord            Keys;
    Command          Does;
    Part             In;          /* where the help puts it */
    std::string_view Says;        /* the help's words */
    std::string_view Named = {};  /* how the help names the key, when not by its chord */
};

/* What a key asks for. Extend: Shift was held on a key that moves. Digit: the
 * digit of a Ctrl and digit. */
template <class Command>
struct Heard {
    Command      Does;
    bool         Extend = false;
    std::uint8_t Digit  = 0;
};

/* A key event as a chord: what it is, its code, and its modifiers; nothing
 * for a key with Alt, which neither program answers. */
struct Pressed {
    bool                        Key   = false;
    std::uint8_t                Code  = 0;
    bool                        Ctrl  = false;
    bool                        Shift = false;
    std::optional<std::uint8_t> Digit;      /* Ctrl and a digit, without Shift */
};

std::optional<Pressed> PressedOf(const tui::KeyEvent &Key) noexcept;
/* Ctrl and a letter is the letter whether Caps Lock was on or not. */
bool                   Fits(const Chord &Keys, const Pressed &Down, bool Shift) noexcept;
std::string            NameOf(const Chord &Keys, std::string_view Named = {});

template <class Command, class Part>
std::optional<Heard<Command>> Hear(std::span<const Binding<Command, Part>> Table, const tui::KeyEvent &Key,
                                   bool (*Moves)(Command) noexcept) noexcept
{
    const std::optional<Pressed> Down = PressedOf(Key);
    if (!Down) return std::nullopt;
    if (Down->Digit)
        for (const Binding<Command, Part> &One : Table)
            if (One.Keys.AnyDigit) return Heard<Command>{One.Does, false, *Down->Digit};

    for (const Binding<Command, Part> &One : Table)
        if (Fits(One.Keys, *Down, Down->Shift)) return Heard<Command>{One.Does};
    if (!Down->Shift) return std::nullopt;
    for (const Binding<Command, Part> &One : Table)
        if (Fits(One.Keys, *Down, false)) return Heard<Command>{One.Does, Moves(One.Does)};
    return std::nullopt;
}

/* A line of the help: a part's heading, or keys and what they do. */
struct KeyLine {
    std::string      Keys;
    std::string_view Says;
    bool             Heading = false;
};

/* The help, from a table of keys: a heading for each part, a line for each
 * binding, and a binding that says nothing of its own named on the line
 * before it. */
template <class Command, class Part>
std::vector<KeyLine> KeyLines(std::span<const Binding<Command, Part>> Table,
                              std::string_view (*PartName)(Part) noexcept)
{
    std::vector<KeyLine>         Lines;
    const Binding<Command, Part> *Last = nullptr;
    std::optional<Part>          In;
    for (const Binding<Command, Part> &One : Table) {
        if (One.Says.empty()) {
            if (Last && Last->Named.empty() && Last->Does == One.Does) Lines.back().Keys += ", " + NameOf(One.Keys);
            continue;
        }
        if (In != One.In) {
            if (In) Lines.push_back({});
            Lines.push_back({std::string(PartName(One.In)), {}, true});
            In = One.In;
        }
        Lines.push_back({NameOf(One.Keys, One.Named), One.Says, false});
        Last = &One;
    }
    return Lines;
}

/* The window of every key, Ctrl+?: two columns side by side where the glass
 * is wide enough, split between two parts; one column that scrolls where it
 * is not. Up and Down scroll it, any other key shuts it. */
class Help {
public:
    enum class Outcome { Stay, Closed };

    void    Begin(std::vector<KeyLine> Lines, std::string_view Program);
    Outcome OnKey(const tui::KeyEvent &Key) noexcept;
    void    Paint(tui::Frame &Out, core::Size Screen) const;

private:
    std::vector<KeyLine> Lines;
    std::string          Program;
    std::size_t          First = 0;
};

}  // namespace kin

#endif
