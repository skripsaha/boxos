/* sprites.h — the animals as little pictures, and which one an animal shows
 * now. A picture faces right, rows of letters, each a colour of the kind's
 * coat ('.' is air; paint_fauna.cpp says what the letters are). What walks has
 * its body drawn without legs and its legs added to the body's hip row, so the
 * stride can turn as slowly or as quickly as the animal goes. */
#ifndef FOREST_SPRITES_H
#define FOREST_SPRITES_H

#include <string_view>

#include "fauna.h"

namespace forest {

struct Sprite {
    int         Height;
    int         Hip;          // the row the legs hang from
    const char *Rows[16];

    int Width() const { return static_cast<int>(std::string_view(Rows[0]).size()); }
};

// Four legs: where the hind and the front pair hang from along the body, and
// how long they are.
struct Legs {
    int Hind;
    int Front;
    int Reach;
};

struct Pose {
    const Sprite *Body;
    const Legs   *Gait;       // none for what sits, lies or flies
    bool          Moving;     // its legs turn with its stride
};

Pose PoseOf(const Animal &Beast);

}  // namespace forest

#endif
