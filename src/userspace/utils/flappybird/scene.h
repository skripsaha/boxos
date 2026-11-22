/* scene.h — the world as the display shows it: the sky, the clouds, the city,
 * the trees and the bushes going by slower than the ground, the pipes, the
 * ground with its grass running past, and the bird — items made once and
 * moved each frame (painter::gui). A unit of the field (world.h) is Unit
 * points.
 *
 * Each layer of the skyline is drawn once into a picture of its own, a
 * period wider than the screen, and the picture slid a whole pixel at a time:
 * the display lays a picture where it is, where a layer of hundreds of rounds
 * it would cover again every frame it moves — and a slow layer moves a pixel
 * only every few frames. */
#ifndef FLAPPYBIRD_SCENE_H
#define FLAPPYBIRD_SCENE_H

#include <initializer_list>
#include <vector>

#include "painter/core/fill.h"
#include "painter/gui/area.h"
#include "painter/gui/blocks.h"
#include "painter/gui/items.h"

#include "look.h"
#include "skyline.h"
#include "world.h"

namespace flappybird {

namespace gui = painter::gui;

class Scene {
public:
    Scene(gui::Area &Screen, const World &Game, const Sky &Shown);
    ~Scene();
    Scene(const Scene &)            = delete;
    Scene &operator=(const Scene &) = delete;

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
    /* A layer of the skyline: its picture, slid by how far the ground has
     * come times its share of that speed. */
    struct Layer {
        gui::Picture Image;
        float        Share   = 0;
        float        Top     = 0;    // in points
        int          Shifted = -1;   // the pixels it is slid by now
    };
    /* What a layer's picture is drawn of: a pattern and how it is filled. */
    struct Piece {
        const skyline::Pattern &Of;
        painter::core::Fill     With;
    };

    void MakeSky(const Sky &Shown);
    void MakeSkyline(const Sky &Shown);
    /* The pieces drawn, one over another, into a picture of the field from
     * Top down to Bottom, in units. */
    gui::Picture Painted(std::initializer_list<Piece> Pieces, float Top, float Bottom);
    void MakeGround(const Sky &Shown);
    void MakeBird();
    void LayPipe(PipePair &Pair, const Pipe &Of);
    void LayBird(const Bird &Of, Stage Now);
    /* An item that stays as it was made: kept, or it would go at once. */
    gui::Shape &Keep(gui::Shape Made);

    gui::Area            &Screen;
    float                 Scale;
    float                 Wide;          // the field's width, in points
    float                 Pixels;        // a unit, in pixels
    float                 Apart;         // a period of the skyline, in whole pixels
    std::vector<gui::Shape> Still;      // what never changes once made
    gui::Shape            Backdrop;
    gui::Shape            Stars;
    std::vector<Layer>    Skyline;
    std::vector<gui::PictureBlock> Blocks;   // the layers' pictures, given back with the scene
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
