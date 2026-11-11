/* keys.h — the keys a person chose, matched and spelled, and the two small
 * questions every playing page asks: whether to leave, and what the keys are. */
#ifndef SEABATTLE_KEYS_H
#define SEABATTLE_KEYS_H

#include <span>
#include <string_view>
#include <string>
#include <utility>

#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "look.h"

namespace seabattle {

// Whether Pressed is the chord the person chose for Which.
bool Chose(const Look &Shown, SeaKey Which, const painter::tui::KeyEvent &Pressed);
// The chord spelled as the person would say it, "ctrl+r".
std::string Spelled(const Look &Shown, SeaKey Which);

// "Leave the game?" over a page, answered by enter or esc.
class LeavePrompt {
public:
    enum class Verdict { Asking, Leave, Stay };

    bool    Open() const { return Asking; }
    void    Ask() { Asking = true; }
    Verdict Answer(const painter::tui::KeyEvent &Pressed);
    void    Paint(painter::tui::Frame &Out, const Look &Shown) const;

private:
    bool Asking = false;
};

using KeyLine = std::pair<std::string, std::string>;

// A panel naming each key and what it does; any key closes it.
void PaintKeys(painter::tui::Frame &Out, const Look &Shown, std::string_view Title, std::span<const KeyLine> Lines);

}  // namespace seabattle

#endif
