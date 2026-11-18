/* words.h — now and then Duppo says a word, in a bubble beside itself: hello
 * as it comes, the time of day, the machine busy or quiet, the odd word of its
 * own. The bubble comes in on a spring, stays a while and fades, a word said
 * over another fading into it; it floats along with Duppo, the display moving
 * the two together. Only on a framebuffer; never when the person has said it
 * should keep quiet. */
#ifndef DUPPO_WORDS_H
#define DUPPO_WORDS_H

#include <chrono>
#include <cstdint>
#include <string_view>

#include "painter/core/geometry.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "vec.h"

namespace duppo {

class Words {
public:
    explicit Words(painter::gui::Area &Screen);

    /* Whether Duppo talks at all, and how often, as a share of the usual. */
    void Tune(bool Talk, float Pace);

    /* Brought to Now (the ClockBoard's milliseconds): a word begun, kept or
     * let go; the bubble laid beside Duppo — its body's top Left and top Right
     * on the glass, in points — and moved there over Glide, as Duppo is. */
    void Step(std::uint64_t Now, painter::core::PointF Left, painter::core::PointF Right, Random &Dice,
              std::chrono::milliseconds Glide);

private:
    std::string_view Pick(Random &Dice);
    void             Say(std::string_view Word);

    painter::gui::Area   &Screen;
    painter::gui::Group   Bubble;
    painter::gui::Shape   Plate;
    painter::gui::Text    Line;
    painter::core::PointF Size{};
    bool                  Talk = true, Saying = false, Greeted = false;
    float                 Pace   = 1.0f;
    std::uint64_t         NextAt = 0, Until = 0;
    std::uint64_t         Busy = 0, Total = 0;   // the machine's, at the last word
};

}  // namespace duppo

#endif
