/* words.h — now and then Duppo says a word, in a bubble beside itself
 * (bubble.h) dressed as the person sets it: hello as it comes, the time of
 * day, the machine busy or quiet, the odd word of its own. The bubble springs
 * out of its tail, stays a while and fades; a word said over another fades
 * into it as the bubble grows or shrinks to it; it floats along with Duppo,
 * its tail pointing at it from whichever side there is room on, and turns to
 * the other side when there is not. A look set anew is taken in time — its
 * colours walking, its shape growing into the new one. Only on a framebuffer;
 * never when the person keeps Duppo quiet. */
#ifndef DUPPO_WORDS_H
#define DUPPO_WORDS_H

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "painter/core/geometry.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "bubble.h"
#include "vec.h"

namespace duppo {

class Words {
public:
    explicit Words(painter::gui::Area &Screen);

    /* Whether Duppo talks at all, how often as a share of the usual, and the
     * bubble it talks in. */
    void Tune(bool Talk, float Often, const BubbleLook &Look);

    /* Brought to Now (the ClockBoard's milliseconds): a word begun, kept or
     * let go; the bubble laid beside Duppo — its body's top Left and top Right
     * on the glass, in points — and moved there over Glide, as Duppo is. */
    void Step(std::uint64_t Now, painter::core::PointF Left, painter::core::PointF Right, Random &Dice,
              std::chrono::milliseconds Glide);

private:
    std::string_view Pick(Random &Dice);
    void             Say(std::string_view Word);
    void             Dress();
    void             Shape();

    painter::gui::Area   &Screen;
    painter::gui::Group   Bubble;
    painter::gui::Shape   Paper;
    painter::gui::Text    Line;
    BubbleLook            Look;
    BubbleShape           Shaped;
    std::string           Said;                       // the word in the bubble
    painter::core::PointF Measured{};
    bool                  TailLeft = true;            // Duppo lies to the bubble's left, below
    bool                  Talk = true, Saying = false, Greeted = false;
    float                 Often  = 1.0f;
    std::uint64_t         NextAt = 0, Until = 0;
    std::uint64_t         Busy = 0, Total = 0;        // the machine's, at the last word
};

}  // namespace duppo

#endif
