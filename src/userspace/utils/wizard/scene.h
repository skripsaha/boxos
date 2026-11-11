/* scene.h — where things stand in the room for a given size of glass, and what
 * the room is shown with in a frame: the picture's pixels are the glass's own,
 * two to a cell high, and every size in the room follows the wizard's. */
#ifndef WIZARD_SCENE_H
#define WIZARD_SCENE_H

#include <cstdint>

#include "numbers.h"

namespace wizard {

struct Box {
    int X, Y, Width, Height;
};

struct Scene {
    int   Cols, Rows;
    int   Width, Height;      // pixels
    int   ShelfTop;           // the pixel row the shelf of things begins at
    int   FloorTop;           // where the wall meets the boards
    int   FeetY;              // where the wizard stands
    float Unit;               // pixels to one of the wizard's own units: he is 100 of them tall
    int   WizardX;            // the middle of him
    int   CauldronX;          // the middle of the pot
    int   CauldronWidth;      // across its rim
    int   PotTop;             // the rim's row
    Box   Window;
    Box   Books;
    int   BookRows;           // how many shelves the books stand on
    Box   Jars;               // the shelf of jars on the wall
    int   CandleX[3];         // the floor candle, the one among the books, the one on the sill
    int   CandleY[3];         // the row each stands on, just above its surface
    int   OwlX, OwlY;
    int   CatX, CatY;
};

// Where everything stands on a glass of Cols by Rows cells.
Scene Arranged(int Cols, int Rows);

// The place of a point of the wizard's own, in his units: his hat's tip is at
// 0 and his feet at 100, his middle at 0 across.
inline float WizardPixelX(const Scene &Where, float Across) { return static_cast<float>(Where.WizardX) + Across * Where.Unit; }
inline float WizardPixelY(const Scene &Where, float Down) { return static_cast<float>(Where.FeetY) - (100.0f - Down) * Where.Unit; }

// How much of the pixels are the picture's: the rows of the bar, the shelf of
// things, the name and the keys that stand under it.
inline constexpr int BarRows = 1;
inline constexpr int ShelfRows = 6;

// What a frame is drawn with.
struct Show {
    std::uint64_t Ms = 0;
    std::uint32_t Brew = 0;         // the liquid's color, 0 for water
    int           Fill = 0;         // things in the cauldron
    float         Excite = 0.0f;    // 1 at the moment of a spell, fading
    float         Stir = 0.0f;      // 1 while he stirs
    float         Splash = 0.0f;    // 1 when a thing has just gone in
    bool          Critters = true;  // the owl and the cat
    Color         Glow;             // what magic glows in
    Color         Fire;
    Color         Candle;
    Color         Moon;
};

}  // namespace wizard

#endif
