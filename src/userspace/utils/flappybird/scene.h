/* scene.h — the world as the display shows it: the sky, the clouds, the city
 * and the bushes going by slower than the ground, the pipes, the ground with
 * its grass running past, and the bird — items made once and moved each
 * frame (painter::gui). A unit of the field (world.h) is Unit points. */
#ifndef FLAPPYBIRD_SCENE_H
#define FLAPPYBIRD_SCENE_H

#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "look.h"
#include "world.h"

namespace flappybird {

namespace gui = painter::gui;

class Scene {
public:
    Scene(gui::Area &Screen, const World &Game, const Sky &Shown);

    float Unit() const noexcept { return Scale; }
    /* Everything that moves where the world says it is now. */
    void  Lay(const World &Game);
    /* The pairs laid at the round's start come up out of nothing. */
    void  ShowPipes(const World &Game);

private:
    struct PipePair {
        gui::Group Whole;
        gui::Shape Upper, UpperCap, Lower, LowerCap;
    };
    /* A layer of the skyline: its picture repeated across the field, slid by
     * how far the ground has come times its share of that speed. */
    struct Layer {
        gui::Group Whole;
        float      Share = 0;
    };

    void MakeSky(const Sky &Shown);
    void MakeSkyline(const Sky &Shown);
    void MakeGround(const Sky &Shown);
    void MakeBird();
    void LayPipe(PipePair &Pair, const Pipe &Of);
    void LayBird(const Bird &Of, Stage Now);
    /* An item that stays as it was made: kept, or it would go at once. */
    gui::Shape &Keep(gui::Shape Made);

    gui::Area            &Screen;
    float                 Scale;
    float                 Wide;          // the field's width, in points
    std::vector<gui::Shape> Still;      // what never changes once made
    gui::Shape            Backdrop;
    gui::Shape            Stars;
    std::vector<Layer>    Skyline;
    gui::Group            Under;          // the pipes, below the ground
    std::vector<PipePair> Pipes;
    gui::Group            Ground;
    gui::Group            Band;           // the grass, clipped
    gui::Shape            Stripes;
    gui::Group            Flier;
    gui::Shape            Wing;
};

}  // namespace flappybird

#endif
