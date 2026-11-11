/* panels.h — the two windows laid over the scene: the list of keys, and the
 * hidden entry of a friend's secret word. */
#ifndef HANGMAN_PANELS_H
#define HANGMAN_PANELS_H

#include <cstddef>
#include <string>

#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "look.h"

namespace hangman {

// The keys as the person set them, and what each does; any key closes it.
void PaintHelp(painter::tui::Frame &Out, const Look &Shown, HintCost Price);

inline constexpr std::size_t LeastSecret = 3;
inline constexpr std::size_t MostSecret = 12;

// A friend typing the secret word: what is typed is shown as stars only.
class SecretEntry {
public:
    enum class Verdict : std::uint8_t { Typing, Done, Cancelled };

    Verdict            Key(const painter::tui::KeyEvent &Pressed);
    void               Paint(painter::tui::Frame &Out) const;
    // The word typed, in capitals.
    const std::string &Word() const { return Typed; }
    void               Clear() { Typed.clear(); }

private:
    std::string Typed;
};

}  // namespace hangman

#endif
