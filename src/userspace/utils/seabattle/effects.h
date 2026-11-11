/* effects.h — the things that happen over the water for a moment: a shell
 * flying in its arc, the splash where it falls, the blast where it hits, the
 * bubbles of a ship going down. Each is drawn from its age alone, so nothing
 * of it is kept between frames, and each has a part in pixels and a part in
 * the finer round shapes. Positions are the canvas's own pixels. */
#ifndef SEABATTLE_EFFECTS_H
#define SEABATTLE_EFFECTS_H

#include <cstdint>
#include <vector>

#include "painter/tui/fine_canvas.h"

#include "canvas.h"

namespace seabattle {

enum class EffectKind : std::uint8_t { Shell, Splash, Blast, Bubbles, Salute };

struct Effect {
    EffectKind    Kind;
    float         X = 0, Y = 0;             // where it happens, or where a shell falls
    float         FromX = 0, FromY = 0;     // where a shell is fired from
    float         Reach = 8;                // pixels to a cell here, or a bubbling ship's width
    float         Length = 0;               // a bubbling ship's length
    std::uint64_t Born = 0;
    std::uint32_t Life = 1000;              // milliseconds
    Color         Tone = Color();           // fire, spray or a firework's colour
};

class Effects {
public:
    void Add(const Effect &One) { Live.push_back(One); }
    void Clear() { Live.clear(); }
    // Whatever has run its time is dropped.
    void Prune(std::uint64_t Now);
    bool Busy() const { return !Live.empty(); }

    void DrawPixels(Canvas &Out, std::uint64_t Now) const;
    void DrawFine(painter::tui::FineCanvas &Out, std::uint64_t Now) const;

private:
    std::vector<Effect> Live;
};

}  // namespace seabattle

#endif
