#ifndef FLAME_H
#define FLAME_H

/* flame — fire on the console, burning from where it is told to.
 *
 * Six parts. flame_sim.cpp is the heat, flame_palette.cpp turns heat into
 * colour, flame_font.cpp draws a word as lit pixels; these three take nothing
 * of BoxOS but its colours, so they run anywhere and what they make can be
 * looked at. The glass is flame_paint.cpp, the menu and keys flame_menu.cpp,
 * and flame.cpp decides: the Form, the clock and the loop.
 *
 * A fire is a grid of heat, one pixel for every half of a cell: two pixels
 * stand in a cell's height. The colours are BoxLib's (box/color.h): a palette
 * is mixed with color_mix and a #rrggbb read with color_parse.
 */

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "box/color.h"

struct TextCell;

enum class FlamePlace : std::uint8_t { Bottom, Top, Sides, Frame, Pit, Word };
enum class FlameInk : std::uint8_t { Fire, Gas, Toxic, Spirit, Custom };

constexpr int FlamePlaceCount = 6;
constexpr int FlameInkCount   = 5;
constexpr int FlameHeightMin  = 1;
constexpr int FlameHeightMax  = 10;
constexpr int FlameWindMax    = 3;
constexpr std::size_t FlameWordMax = 32;

struct FlameSettings {
    FlamePlace    Place     = FlamePlace::Bottom;
    FlameInk      Ink       = FlameInk::Fire;
    std::uint32_t Custom    = 0xFF6A00;
    bool          HasCustom = false;
    int           Height    = 5;
    int           Wind      = 0;
    bool          Sparks    = false;
    std::string   Word;
};

const char *FlamePlaceName(FlamePlace Place);
const char *FlameInkName(FlameInk Ink);

using FlamePalette = std::array<std::uint32_t, 256>;

FlamePalette FlameMakePalette(FlameInk Ink, std::uint32_t Custom);

/* The word as lit pixels on a Width x Height grid, centred, as big as fits. */
std::vector<std::uint8_t> FlameWordPixels(const std::string &Word, int Width, int Height);

struct FlameStepDir {
    std::int8_t X = 0;
    std::int8_t Y = 0;
};

/* A source cell sparks may leave from, and the way away from the source. */
struct FlameSpawn {
    int          X;
    int          Y;
    FlameStepDir Away;
};

/* One body of fire: its own sources, its own way heat is pulled. A word burns
 * in a layer of its own, rising, whatever the placement does. */
struct FlameLayer {
    std::vector<std::uint8_t>  Heat, Next, Source, Cool;
    std::vector<FlameStepDir>  Pull;
    std::vector<FlameSpawn>    Spawns;
    int                        Extent      = 1;
    int                        SparkChance = 10;
    int                        Surge       = 140;
};

struct FlameSpark {
    float X, Y, Vx, Vy, Life, Span;
};

class FlameFire {
public:
    static constexpr int TurbulenceSide = 256;
    static constexpr unsigned TickUs = 25000;

    FlameFire(int Width, int Height, std::uint64_t Seed);

    void Rebuild(const FlameSettings &Settings);
    void Step(const FlameSettings &Settings);

    int          Width() const { return Wide; }
    int          Height() const { return High; }
    std::uint8_t At(int X, int Y) const { return Out[static_cast<std::size_t>(Y) * Wide + X]; }

private:
    std::uint64_t Roll();
    FlameLayer    PlaceLayer(FlamePlace Place);
    FlameLayer    WordLayer(const std::string &Word);
    FlameLayer    EmptyLayer(int Extent);
    void          MakeTurbulence();
    int           Turbulence(FlameStepDir Pull, int X, int Y) const;
    void          StepLayer(FlameLayer &Layer, int Key, const FlameSettings &Settings);
    void          StepSparks(const FlameSettings &Settings);
    void          Spawn(const FlameLayer &Layer, int Chance);

    int                         Wide;
    int                         High;
    std::uint64_t               Seed;
    std::vector<FlameLayer>     Layers;
    std::vector<FlameSpark>     Sparks;
    std::vector<std::uint8_t>   Out;
    std::vector<std::uint8_t>   Noise;
    unsigned                    Clock = 0;
};

struct FlameKey {
    char          Ascii;
    bool          Ctrl;
    bool          Extended;
    std::uint8_t  Scancode;
};

struct FlameOutcome {
    bool Leave     = false;
    bool Rebuild   = false;
    bool Repalette = false;
};

class FlameMenu {
public:
    bool Open = false;

    FlameOutcome OnKey(const FlameKey &Key, FlameSettings &Settings);
    void         Draw(TextCell *Frame, int Rows, int Cols, const FlameSettings &Settings) const;
    void         Caret(int Rows, int Cols, int &Row, int &Col) const;

private:
    FlameOutcome Quick(const FlameKey &Key, FlameSettings &Settings);
    FlameOutcome Choose(const FlameKey &Key, FlameSettings &Settings);
    FlameOutcome Type(const FlameKey &Key, FlameSettings &Settings);
    FlameOutcome Apply(int Row, int Delta, FlameSettings &Settings);
    FlameOutcome SetPlace(FlamePlace Place, FlameSettings &Settings);
    FlameOutcome Accept(FlameSettings &Settings);
    void         BeginEdit(const FlameSettings &Settings);

    bool        Editing = false;
    int         Item    = 0;
    const char *Note    = nullptr;
    std::string Text;
};

constexpr int FlameMinCols = 56;
constexpr int FlameMinRows = 14;

/* Two pixels to a cell: the upper half block, ink the pixel above, paper the
 * pixel below. */
void FlamePaintFire(const FlameFire &Fire, const FlamePalette &Palette, TextCell *Frame, int Rows, int Cols);

#endif
