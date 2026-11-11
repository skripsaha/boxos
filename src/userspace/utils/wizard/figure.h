/* figure.h — the wizard: robe, beard, hat and face drawn once at the glass's own
 * half-block pixels with the shading of folds and strands, and every frame
 * blitted with the sway of the beard, the hem and the hat's tip, the rise of a
 * breath and a turn of the head, over which his eyes, his mouth, his arms and
 * his staff are drawn live. */
#ifndef WIZARD_FIGURE_H
#define WIZARD_FIGURE_H

#include "canvas.h"
#include "cauldron.h"
#include "light.h"
#include "lore.h"
#include "scene.h"

namespace wizard {

// What he is doing at this moment, each part from 0 to 1.
struct Pose {
    float Look = 0.0f;         // where his eyes turn, -1 left to 1 right
    float LookUp = 0.0f;       // -1 down to 1 up
    float Speak = 0.0f;        // how far his mouth is open
    float Blink = 0.0f;        // how far his eyes are shut
    Mood  Face = Mood::Pleased;
    float FaceWeight = 0.0f;   // how much of the mood is on his face
    float Stir = 0.0f;         // the ladle in the pot
    float Cast = 0.0f;         // staff raised, hand open
    float Drink = 0.0f;        // the vial at his mouth
    float Spin = 0.0f;         // turns of the ladle, and of a dizzy eye
    float Breath = 0.0f;       // 0 to 1 and back
};

class Figure {
public:
    // The sprites for Where, in the colors of the settings.
    void Build(const Scene &Where, const Look &Shown);
    void Draw(Canvas &Out, const Scene &Where, const Pose &Now, const Show &Shown, const Lighting &Lights);

private:
    Sprite Back;     // shoes, robe and what is on it
    Sprite Front;    // head, hat and beard
};

}  // namespace wizard

#endif
