#ifndef AQUARIUM_H
#define AQUARIUM_H

/* aquarium — a tank of water behind the glass of the console.
 *
 * Five parts. aquarium_kinds.cpp is what lives in the tank and how each
 * looks; aquarium_life.cpp is how they move; aquarium_scene.cpp is what
 * stands still — sand, rocks, coral, plants, the chest. aquarium_paint.cpp is
 * the picture through the glass, and aquarium.cpp decides: the Form, the
 * keys, the clock, the loop. Only aquarium.cpp and aquarium_paint.cpp know
 * BoxOS's screen; the rest takes nothing of it but colours.
 *
 * The tank is a grid of pixels, two to a cell's height (the upper half block),
 * wider and deeper than the screen: the glass shows a window onto it, and the
 * arrows move the window. Positions are in pixels, times in seconds.
 */

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "box/color.h"

struct TextCell;

/* ---- numbers ------------------------------------------------------------ */

/* The tank's own dice: xorshift, seeded once, so a run is its own. */
class AquaDice {
public:
    explicit AquaDice(std::uint64_t Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t Next();
    float         Unit();                       /* [0, 1) */
    float         Between(float Low, float High);
    int           Count(int Low, int High);     /* [Low, High] */
    bool          Chance(float Probability);

private:
    std::uint64_t State;
};

/* A wave, -1..1, of `Turns` whole turns: a sine read from a table made once,
 * so a frame of thousands of them costs no more than its arithmetic. */
float AquaWave(float Turns);
void  AquaMakeWaves();

/* `Light` 0..256 of the colour; 256 leaves it as it is. */
Color AquaShade(Color Ink, int Light);

/* ---- what lives in the tank --------------------------------------------- */

/* How a kind moves. */
enum class AquaWay : std::uint8_t {
    Swims,       /* anywhere in its band of depth */
    Schools,     /* swims in a school behind a leader */
    Guards,      /* keeps close to the anemone, its home */
    Drifts,      /* a jellyfish: pulses up, sinks slowly */
    Hovers,      /* a seahorse: upright, bobbing by the plants */
    Walks,       /* along the sand, sideways */
    Creeps,      /* along the sand, slowly */
    Rests,       /* on the sand, still */
    Visits       /* crosses the tank once, now and then, and leaves */
};

/* A picture of a creature facing right: rows of letters, each a colour of its
 * look — '.' is water. b body, s second (stripe, belly), f fin and tail,
 * e eye, d dark, w white, g glow. */
struct AquaSprite {
    int                      Width  = 0;
    int                      Height = 0;
    std::vector<std::string> Rows;
};

struct AquaLook {
    Color Body;
    Color Second;
    Color Fin;
    Color Dark;
    Color Glow;
};

struct AquaKind {
    const char             *Name;
    AquaWay                 Way;
    std::vector<AquaSprite> Frames;      /* the frames of its stroke, in turn */
    std::vector<AquaSprite> Puffed;      /* a pufferfish, frightened */
    AquaLook                Look;
    float                   Speed;       /* pixels a second, cruising */
    float                   Top;         /* its band of depth, 0 the surface .. 1 the sand */
    float                   Bottom;
    int                     Fewest;      /* how many a tank of a screen holds */
    int                     Most;
    bool                    Glows;       /* its g pixels shine in the dark */
};

const std::vector<AquaKind> &AquaKinds();

/* ---- the tank ------------------------------------------------------------ */

enum class AquaLight : std::uint8_t { Clock, Day, Dusk, Night };

const char *AquaLightName(AquaLight Light);

struct AquaCreature {
    int   Kind;
    float X, Y;            /* its top-left pixel */
    float Vx = 0, Vy = 0;
    float GoalX = 0, GoalY = 0;
    float GoalLeft = 0;    /* seconds before it picks another goal */
    float Stroke = 0;      /* turns of its stroke, for the frames */
    float Phase = 0;       /* its own place in every slow wave, so no two drift alike */
    float Hunger = 0;      /* 0 fed .. 1 hungry */
    float Fright = 0;      /* seconds of fleeing left */
    float Rest = 0;        /* seconds of standing still left */
    int   Leader = -1;     /* the one it schools behind */
    float OffsetX = 0, OffsetY = 0;
    bool  Left = false;    /* faces left */
    bool  Gone = false;    /* a visitor that has left */
};

struct AquaBubble {
    float X, Y;
    float Rise;            /* pixels a second */
    float Phase;
    bool  Big;
};

struct AquaFlake {
    float X, Y;
    float Phase;
    float Lying = 0;       /* seconds on the sand */
    Color Ink;
};

struct AquaStalk {         /* kelp or grass, rooted in the sand */
    int   X;
    int   Height;
    float Phase;
    Color Ink;
    bool  Kelp;
};

struct AquaCoral {
    int                       X, Y, Width, Height;
    std::vector<std::uint8_t> Pixels;   /* 0 none, 1 coral, 2 its tips */
    Color                     Ink, Tip;
};

struct AquaRock {
    int   X, Y, Width, Height;
    bool  Cave;
    Color Ink;
};

struct AquaShell {
    int   X, Y;
    Color Ink;
};

struct AquaChest {
    int   X = 0, Y = 0;
    float Open = 0;        /* 0 shut .. 1 open */
    float Wait = 0;        /* seconds before it opens */
    float Held = 0;        /* seconds it stays open */
};

/* Everything that stands still, laid once (aquarium_scene.cpp). */
struct AquaScene {
    std::vector<int>       Floor;       /* the sand's top, a pixel row for each column */
    std::vector<AquaStalk> Stalks;
    std::vector<AquaCoral> Corals;
    std::vector<AquaRock>  Rocks;
    std::vector<AquaShell> Shells;
    std::vector<int>       Far;         /* the far hills' height, for each column of their own */
    AquaChest              Chest;
    int                    AnemoneX = 0, AnemoneY = 0;
    int                    BubblerX = 0, BubblerY = 0;
};

struct AquaTank {
    int                       Width  = 0;
    int                       Height = 0;
    int                       Surface = 0;   /* the water's top row */
    AquaScene                 Scene;
    std::vector<AquaCreature> Creatures;
    std::vector<AquaBubble>   Bubbles;
    std::vector<AquaFlake>    Flakes;
    float                     Time = 0;
    float                     SinceBubble = 0;   /* seconds since the air stone's last bubble */
    float                     VisitorWait = 0;
    unsigned                  Fed = 0;
};

/* A tank for a window of ViewWidth x ViewHeight pixels: wider and deeper, with
 * `Fish` creatures that swim (0: as many as its size holds). */
AquaTank AquaMakeTank(int ViewWidth, int ViewHeight, int Fish, AquaDice &Dice);
void     AquaMakeScene(AquaTank &Tank, AquaDice &Dice);

/* The sand's top at column X, and how deep the sand lies under its dunes. */
int AquaFloor(const AquaTank &Tank, int X);
int AquaSandDepth(const AquaTank &Tank);

/* Where in the water a band of depth (0 the surface .. 1 the sand) is, for a
 * creature `Height` tall at column X. */
float AquaBandY(const AquaTank &Tank, float Band, int Height, int X);

/* A creature of kind `Kind` born at column X, in its band of depth. */
AquaCreature AquaHatch(const AquaTank &Tank, int Kind, float X, AquaDice &Dice);

/* `Seconds` of life (aquarium_life.cpp). */
void AquaStep(AquaTank &Tank, float Seconds, AquaDice &Dice);
/* Food over (X, the surface): a pinch of flakes. */
void AquaFeed(AquaTank &Tank, float X, AquaDice &Dice);
/* A knock on the glass in front of (X, Y): what swims there flees from it. */
void AquaTap(AquaTank &Tank, float X, float Y);
/* A creature arrives, of a kind that swims, in front of (X, Y). */
void AquaAdd(AquaTank &Tank, float X, float Y, AquaDice &Dice);

/* The sprite a creature shows now. */
const AquaSprite &AquaFrameOf(const AquaCreature &Creature);

/* ---- the picture --------------------------------------------------------- */

/* How much light there is, 0 dark .. 256 noon, and how the water is lit. */
struct AquaDaylight {
    int  Sun   = 256;
    bool Night = false;
};

AquaDaylight AquaDaylightAt(AquaLight Light, int Hour, int Minute);

struct AquaView {
    int   Width, Height;   /* pixels */
    float X, Y;            /* its top-left in the tank */
};

/* The window onto the tank, as pixels, ViewWidth x ViewHeight, row by row. */
void AquaPaint(const AquaTank &Tank, const AquaView &View, const AquaDaylight &Daylight,
               std::vector<Color> &Pixels);

/* Two pixels a cell: the upper half block, ink the pixel above. */
void AquaCells(const std::vector<Color> &Pixels, int Width, TextCell *Frame, int Rows, int Cols);

#endif
