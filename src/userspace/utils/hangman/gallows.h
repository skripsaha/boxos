/* gallows.h — the gallows and the man, in the scene's units. The gallows is
 * built in four pieces (the stage, the post, the beam, the rope), and the man in
 * six, one for each mistake: the head, the body, an arm, the other arm, a leg,
 * the other. Each piece is drawn as far as its progress, 0 to 1, has come. */
#ifndef HANGMAN_GALLOWS_H
#define HANGMAN_GALLOWS_H

#include <array>

#include "painter/tui/fine_canvas.h"

#include "tones.h"
#include "view.h"

namespace hangman {

inline constexpr int PieceCount = 4;
inline constexpr int PartCount = 6;

// Where the timber stands, by how wide the scene is.
struct Timbers {
    float Post;          // the post's middle
    float StageLeft;
    float StageRight;
    float BeamEnd;
    float RopeAt;        // where the rope hangs
    float Pivot;         // the middle of it all, on the stage's floor
    static constexpr float StageTop = 80.0f;
    static constexpr float BeamTop = 10.0f;
    static constexpr float BeamBottom = 14.0f;

    static Timbers For(float Across);
};

// How the man stands or hangs in this frame.
enum class Mouth : std::uint8_t { Sad, Plain, Smile };

struct Posture {
    std::array<float, PieceCount> Built{};    // the stage, the post, the beam, the rope
    std::array<float, PartCount>  Part{};     // the head, the body, the arms, the legs
    float Swing = 0.0f;                       // radians from the straight down, to the right
    float Fall = 0.0f;                        // 0 hanging, 1 freed and stood on the stage
    float Jump = 0.0f;                        // how high he is off the stage, in the scene's units
    float Cheer = 0.0f;                       // 0 arms down, 1 arms up and waving
    float Seconds = 0.0f;                     // time, for the waving and the jumping
    float Rope = 1.0f;                        // how much of the rope is left
    Mouth Face = Mouth::Plain;
};

void PaintGallows(painter::tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone,
                  const Posture &Now);
void PaintMan(painter::tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone,
              const Posture &Now);

}  // namespace hangman

#endif
