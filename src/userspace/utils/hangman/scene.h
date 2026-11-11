/* scene.h — the picture of a round: the hill, the sky and the gallows, and what
 * happens in them. The gallows is built at the start of a word, each mistake
 * puts up a part of the man, and the end of the round changes the hour: a win
 * cuts the rope and the man drops to the stage and cheers under confetti, a
 * loss lets the sun set over a still gallows. Time comes from outside, and
 * what the scene knows of the game is only these calls. */
#ifndef HANGMAN_SCENE_H
#define HANGMAN_SCENE_H

#include <array>
#include <cstdint>
#include <optional>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "flurry.h"
#include "gallows.h"
#include "look.h"
#include "luck.h"
#include "tones.h"

namespace hangman {

class Scene {
public:
    // A new word: the gallows is built anew, in daylight.
    void Begin(std::uint64_t Now);
    // The mistake that puts up the part Index (0 the head to 5 the last leg).
    void Mistake(int Index, std::uint64_t Now);
    // The man is freed.
    void Free(std::uint64_t Now);
    // The man is lost.
    void Lose(std::uint64_t Now);

    // Whether something is moving that the person watches, and the picture is worth drawing often.
    bool Busy(std::uint64_t Now) const;

    void Step(std::uint32_t ElapsedMs, std::uint64_t Now, Luck &Chance, const Look &Shown, painter::core::Rect Cells);
    // The scene drawn in a frame of its own, so nothing reaches past Cells, and copied to Out there.
    void Paint(painter::tui::Frame &Out, painter::core::Rect Cells, const Look &Shown, std::uint64_t Now);

private:
    enum class Ending : std::uint8_t { None, Freed, Lost };

    void    PaintBackground(painter::tui::Frame &Page, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now);
    Posture Posed(std::uint64_t Now) const;
    float   SecondsSince(std::uint64_t Moment, std::uint64_t Now) const;
    void    Cheer(Luck &Chance, float Seconds, const Look &Shown, const View &Where);

    std::array<std::uint64_t, PartCount> PartAt{};   // when each part began to appear; 0 for not yet
    std::uint64_t Begun = 0;
    std::uint64_t Ended = 0;
    Ending        Over = Ending::None;
    int           Appeared = 0;
    int           Cheers = 0;
    float         Dusk = 0.0f;
    float         Joy = 0.0f;
    float         Angle = 0.0f;       // the swing, and how fast it changes
    float         Speed = 0.0f;
    std::uint32_t LeafMs = 0;
    Flurry        Flying;
    std::optional<painter::tui::Frame> Canvas;
    std::optional<painter::tui::Frame> Backdrop;   // the sky and the hill as last drawn, while they stand still
    Tones                              Painted{};
};

}  // namespace hangman

#endif
